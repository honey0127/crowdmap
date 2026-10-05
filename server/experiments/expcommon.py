"""실험 스크립트 공용 함수 (표준 라이브러리만 사용).

- 보정값(calib.env) 읽기, 도착률 계산(1코어 기준 이용률 → λ)
- 실행 결과 읽기: meta.env, 부하 생성기 요약(lg_summary.csv), 컨트롤러 기록(ctrl.csv)
- 대기행렬 계산(Erlang C), 반복 평균과 95% 신뢰구간
"""
import csv
import math
import os
import statistics

NAN = float("nan")

# t 분포 양측 95% 임계값 (자유도 1~30). 그 이상은 1.96
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306,
       9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120,
       17: 2.110, 18: 2.101, 19: 2.093, 20: 2.086, 21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064,
       25: 2.060, 26: 2.056, 27: 2.052, 28: 2.048, 29: 2.045, 30: 2.042}


def fnum(x, default=NAN):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def isnum(x):
    return isinstance(x, (int, float)) and not math.isnan(x)


def mean_ci(values):
    """반복 평균, 95% 신뢰구간 반폭, 표본 수. 표본 1개면 반폭은 NaN."""
    v = [x for x in values if isnum(x)]
    n = len(v)
    if n == 0:
        return NAN, NAN, 0
    m = sum(v) / n
    if n == 1:
        return m, NAN, 1
    return m, T95.get(n - 1, 1.96) * statistics.stdev(v) / math.sqrt(n), n


def fmt(x, digits=2):
    if not isnum(x):
        return "-"
    return f"{x:.{digits}f}"


def fmt_ci(m, h, digits=2):
    if not isnum(m):
        return "-"
    if not isnum(h):
        return fmt(m, digits)
    return f"{m:.{digits}f} ± {h:.{digits}f}"


# ── 파일 읽기 ────────────────────────────────────────────────────────────

def read_kv(path):
    """KEY=VALUE 파일 (따옴표·# 주석 허용)."""
    d = {}
    if not os.path.exists(path):
        return d
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            v = v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            d[k.strip()] = v
    return d


def read_summary(path):
    """lg_summary.csv → {class: {열: 값}} (숫자는 float)."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            out[row["class"]] = {k: (fnum(v, v) if k not in ("label", "class", "work_dist") else v)
                                 for k, v in row.items()}
    return out


def read_ts(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [{k: fnum(v) for k, v in row.items()} for row in csv.DictReader(f)]


def measurement_window(summary_row, meta):
    """부하 생성기 측정 구간을 서버 시계(epoch)로 바꾼 [t0, t1].
    CLOCK_OFFSET_S = 서버 시계 − 생성기 시계 (preflight 에서 잼, 같은 기계면 0)."""
    t0 = summary_row.get("t0_epoch", NAN)
    w = summary_row.get("warmup_s", NAN)
    d = summary_row.get("duration_s", NAN)
    off = fnum(meta.get("CLOCK_OFFSET_S", "0"), NAN)
    if not (isnum(t0) and isnum(w) and isnum(d) and isnum(off)):
        return None
    if abs(off) > 60:   # ssh 실패로 생긴 터무니없는 값: 구간을 틀리게 자르느니 컨트롤러 지표를 비운다
        return None
    return t0 + w + off, t0 + w + d + off


def ctrl_window_metrics(path, window):
    """ctrl.csv 를 측정 구간으로 잘라 종류별 지표를 낸다."""
    if window is None or not os.path.exists(path):
        return {}
    t0, t1 = window
    rows = {}
    with open(path, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            e = fnum(r.get("epoch_s"))
            if not isnum(e) or e < t0 or e > t1:
                continue
            rows.setdefault(r["cls"], []).append(r)
    out = {}
    for cls, rs in rows.items():
        if len(rs) < 2:
            continue
        g = lambda r, k: fnum(r.get(k))
        span = g(rs[-1], "epoch_s") - g(rs[0], "epoch_s")
        cs = [g(r, "c") for r in rs]
        changes = sum(1 for a, b in zip(cs, cs[1:]) if a != b)
        done = sum(g(r, "d_done") for r in rs)
        svc = sum(g(r, "svc_us") * g(r, "d_done") for r in rs)
        wait = sum(g(r, "wait_ms") * g(r, "d_done") for r in rs)
        mus = sorted(g(r, "mu") for r in rs if g(r, "d_done") > 0)
        out[cls] = {
            "span_s": span,
            "mean_c": sum(cs) / len(cs),
            "max_c": max(cs),
            "changes_per_min": changes / span * 60 if span > 0 else NAN,
            "cpu_cores": (g(rs[-1], "cpu_s") - g(rs[0], "cpu_s")) / span if span > 0 else NAN,
            "reactor_util": (g(rs[-1], "reactor_cpu_s") - g(rs[0], "reactor_cpu_s")) / span
            if span > 0 else NAN,
            "proc_cores": (g(rs[-1], "proc_cpu_s") - g(rs[0], "proc_cpu_s")) / span
            if span > 0 else NAN,
            "offered": sum(g(r, "d_off") for r in rs),
            "shed": sum(g(r, "d_shed") for r in rs),
            "done": done,
            "expired": sum(g(r, "d_exp") for r in rs),
            "svc_us": svc / done if done else NAN,
            "wait_ms": wait / done if done else NAN,
            "max_q": max(g(r, "q") for r in rs),
            "mean_q": sum(g(r, "q") for r in rs) / len(rs),
            "mean_k": sum(g(r, "k") for r in rs) / len(rs),
            "mu_median": mus[len(mus) // 2] if mus else NAN,
            "lam_mean": sum(g(r, "lam") for r in rs) / len(rs),
        }
    return out


def load_run(run_dir):
    """실행 1회: meta, 요약(종류별), 컨트롤러 지표(종류별), 시계열. 완료(DONE)가 아니면 None."""
    if not os.path.exists(os.path.join(run_dir, "DONE")):
        return None
    meta = read_kv(os.path.join(run_dir, "meta.env"))
    summ = read_summary(os.path.join(run_dir, "lg_summary.csv"))
    if "ALL" not in summ:
        return None
    window = measurement_window(summ["ALL"], meta)
    ctrl = ctrl_window_metrics(os.path.join(run_dir, "ctrl.csv"), window)
    return {"dir": run_dir, "meta": meta, "summary": summ, "ctrl": ctrl, "window": window}


def plan_ids(exp_dir):
    """실험 디렉터리의 plan*.txt 에 있는 run_id 집합. 계획 파일이 없으면 None."""
    if not os.path.isdir(exp_dir):
        return None
    ids, found = set(), False
    for name in os.listdir(exp_dir):
        if name.startswith("plan") and name.endswith(".txt"):
            found = True
            with open(os.path.join(exp_dir, name), encoding="utf-8") as f:
                ids.update(line.split("|", 1)[0] for line in f if line.strip())
    return ids if found else None


def load_runs(exp_dir, only=None):
    """완료된 실행을 읽는다. only(run_id 집합)가 있으면 그 실행만 읽고, 나머지 완료 실행 이름은
    load_runs.extra 에 남긴다(지금 계획에 없는 예전 결과 — 보고서에 경고로 싣는다).
    '_' 로 시작하는 디렉터리(_stale, _probe)는 건너뛴다."""
    runs, extra = [], []
    if os.path.isdir(exp_dir):
        for name in sorted(os.listdir(exp_dir)):
            d = os.path.join(exp_dir, name)
            if not os.path.isdir(d) or name.startswith("_"):
                continue
            if only is not None and name not in only:
                if os.path.exists(os.path.join(d, "DONE")):
                    extra.append(name)
                continue
            r = load_run(d)
            if r:
                runs.append(r)
    load_runs.extra = extra
    return runs


load_runs.extra = []


# ── 보정값과 도착률 ──────────────────────────────────────────────────────

class Calib:
    """exp0 결과. SIMPLE 처리 시간은 도착률에 따라 변하므로 (도착률, S) 표로 갖는다."""

    def __init__(self, path):
        kv = read_kv(path)
        if not kv:
            raise FileNotFoundError(f"보정 파일 없음 또는 비어 있음: {path} (exp0_calibrate.sh 먼저)")
        self.kv = kv
        table = []
        for item in kv.get("CAL_SIMPLE_TABLE", "").split():
            r, s = item.split(":")
            table.append((float(r), float(s)))
        if not table:
            raise ValueError("CAL_SIMPLE_TABLE 이 비어 있음")
        self.simple_table = sorted(table)
        self.s_complex_us = float(kv["CAL_S_COMPLEX_US"])
        self.work_us = float(kv.get("CAL_WORK_US", "10000"))
        self.reactor_limit = float(kv["CAL_REACTOR_LIMIT"])
        self.reactor_kind = kv.get("CAL_REACTOR_LIMIT_KIND", "measured")
        self.mu0_simple = float(kv.get("MU0_SIMPLE", "33333"))
        self.mu0_complex = float(kv.get("MU0_COMPLEX", "100"))

    def s_simple(self, rate):
        """도착률 rate 에서의 SIMPLE 처리 시간(µs). 로그 도착률에 대해 선형 보간, 범위 밖은 끝값."""
        t = self.simple_table
        if rate <= t[0][0]:
            return t[0][1]
        if rate >= t[-1][0]:
            return t[-1][1]
        for (r0, s0), (r1, s1) in zip(t, t[1:]):
            if r0 <= rate <= r1:
                x = (math.log(rate) - math.log(r0)) / (math.log(r1) - math.log(r0))
                return s0 + (s1 - s0) * x
        return t[-1][1]

    def s_complex(self, work_us):
        """COMPLEX 처리 시간: 보정 때 잰 값에서 작업량 차이만큼 옮긴다(오버헤드 동일 가정)."""
        return self.s_complex_us - self.work_us + work_us


def complex_frac_for_share(share, s_s, s_c):
    """COMPLEX 가 CPU 의 share 를 차지하는 비율 p:  p·S_c / ((1−p)·S_s + p·S_c) = share."""
    if share <= 0:
        return 0.0
    if share >= 1:
        return 1.0
    return share * s_s / (share * s_s + (1 - share) * s_c)


def rate_for(cal, rho, share, work_us):
    """1코어 기준 이용률 rho 가 되는 평균 도착률.  λ·E[S] = rho,  E[S] = (1−p)S_s(λ_s) + p·S_c.
    S_s 가 SIMPLE 도착률 λ_s = (1−p)λ 에 따라 변하므로 고정점 λ = rho / E[S(λ)] 를 반복으로 푼다.
    (S_s 는 λ 가 커질수록 줄어들지만 λ·S_s(λ) 는 늘어나 해가 하나다. 반씩 섞어 진동을 막는다.)"""
    s_c = cal.s_complex(work_us)

    def at(lam):
        p = complex_frac_for_share(share, cal.s_simple(lam), s_c)
        s_s = cal.s_simple((1 - p) * lam)
        p = complex_frac_for_share(share, s_s, s_c)
        return p, s_s, (1 - p) * s_s + p * s_c

    lam = rho / (cal.s_simple(cal.simple_table[len(cal.simple_table) // 2][0]) * 1e-6)
    for _ in range(1000):
        new = rho / (at(lam)[2] * 1e-6)
        if abs(new - lam) <= 1e-9 * lam:
            break
        lam = 0.5 * lam + 0.5 * new
    p, s_s, es = at(lam)
    if abs(lam * es * 1e-6 - rho) > 1e-3 * rho:
        raise ValueError(f"도착률 계산이 수렴하지 않음: rho={rho} share={share}")
    return {"rate": lam, "p": p, "s_simple_us": s_s, "s_complex_us": s_c, "es_us": es}


def peak_factor(f, period, width):
    """동기화 비율 f, 주기 P, 폭 w 일 때 순간 최대/평균."""
    if f <= 0 or width <= 0:
        return 1.0
    return f * period / width + (1 - f)


# ── 대기행렬 ─────────────────────────────────────────────────────────────

def erlang_c(c, a):
    if a <= 0:
        return 0.0
    if a >= c:
        return 1.0
    b = 1.0
    for k in range(1, c + 1):
        b = a * b / (k + a * b)
    rho = a / c
    return b / (1 - rho + rho * b)


def p_wait_gt(c, lam, mu, t):
    """M/M/c: P(Wq > t) = C(c,a)·exp(−(cμ−λ)t). 불안정(λ ≥ cμ)이면 1."""
    a = lam / mu
    if a >= c:
        return 1.0
    return erlang_c(c, a) * math.exp(-(c * mu - lam) * t)


def min_servers_erlang(lam, mu, t, p, rho_max, c_max=64):
    for c in range(1, c_max + 1):
        if lam / (c * mu) > rho_max:
            continue
        if p_wait_gt(c, lam, mu, t) <= p:
            return c
    return None


def min_servers_naive(lam, mu, rho_max):
    return max(1, math.ceil(lam / (mu * rho_max) - 1e-9))
