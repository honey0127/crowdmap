#!/usr/bin/env python3
"""실험 실행 목록 만들기.

사용: plan.py runs <0simple|0complex|0reactor|A|B|C|D>   → 표준출력에 실행 목록, 표준오류에 요약
      plan.py rate <rho> <share>                         → 보정값으로 계산한 λ, p (확인용)

실행 목록: 한 줄에 한 실행, '|' 구분 — run_id|server_env|lg_args|expected_s|meta(공백 구분 K=V)
(탭으로 나누면 bash read 가 빈 칸을 합쳐 열이 밀리므로 공백류가 아닌 '|' 를 쓴다)
설정은 lib.sh 가 export 한 환경변수로 받는다(기본값은 아래 env() 호출의 두 번째 인자).

실행 순서: 반복(rep) 하나 안에서 조건을 섞는다(반복마다 다른 고정 시드). 시간에 따른 변화
(서버 온도, 다른 VM 의 소음)가 특정 조건에만 몰리지 않게 하기 위해서다.
같은 반복 안의 실행은 부하 생성기 시드가 같다 → B 처럼 설정만 다른 비교는 같은 도착 순서로 짝지어진다.
"""
import os
import random
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from expcommon import Calib, peak_factor, rate_for  # noqa: E402

ENV_TOKEN = re.compile(r"^[A-Z][A-Z0-9_]*=[A-Za-z0-9_.,:/+-]*$")


def env(name, default):
    v = os.environ.get(name)
    return default if v is None or v == "" else v


def envf(name, default):
    return float(env(name, str(default)))


def envi(name, default):
    return int(float(env(name, str(default))))


def envlist(name, default):
    return env(name, default).split()


def check_env(s):
    """서버 환경변수 문자열은 원격 셸 명령에 그대로 들어가므로 안전한 토큰만 허용한다."""
    for tok in s.split():
        if not ENV_TOKEN.match(tok):
            sys.exit(f"[plan] 허용하지 않는 서버 환경변수 토큰: {tok!r}")
    return s


def die(msg):
    sys.exit(f"[plan] {msg}")


def calib():
    path = env("CALIB_FILE", "")
    if not path:
        die("CALIB_FILE 이 설정되지 않음")
    try:
        return Calib(path)
    except (FileNotFoundError, KeyError, ValueError) as e:
        die(str(e))


def mu0_env(cal):
    return f"MU0_SIMPLE={cal.mu0_simple:.0f} MU0_COMPLEX={cal.mu0_complex:.2f}"


def expected_seconds(conns, warmup, duration):
    start = envf("START_OVERHEAD_S", 4)
    cool = envf("COOLDOWN_S", 3)
    timeout = envf("TIMEOUT", 3)
    return start + conns / 5000.0 + 1 + warmup + duration + timeout + 0.5 + cool


def lg_common(conns, warmup, duration, seed):
    return (f"--conns {conns} --warmup {warmup:g} --duration {duration:g} "
            f"--sync-width {envf('SYNC_WIDTH', 3):g} --sync-period {envf('SYNC_PERIOD', 25):g} "
            f"--work-us {envf('WORK_US', 10000):g} --q-threshold-ms {envf('Q_THRESHOLD_MS', 50):g} "
            f"--seed {seed}")


def order(exp, conds, reps):
    """[(cond, rep)] — 반복 단위로 묶고, 반복 안에서는 고정 시드로 섞는다."""
    base = envi("SEED_BASE", 1)
    out = []
    for rep in range(1, reps + 1):
        cs = list(conds)
        random.Random(f"{exp}-{rep}-{base}").shuffle(cs)
        out.extend((c, rep) for c in cs)
    return out


def seed_for(rep):
    return envi("SEED_BASE", 1) * 1000 + rep


def emit(rows, excluded, title, notes=()):
    total = 0.0
    for r in rows:
        meta = " ".join(f"{k}={v}" for k, v in r["meta"].items())
        fields = [r["run_id"], check_env(r["server_env"]), r["lg_args"],
                  f"{r['expected_s']:.0f}", meta]
        if any("|" in x for x in fields):
            die(f"필드에 '|' 가 들어감: {fields}")
        print("|".join(fields))
        total += r["expected_s"]
    sys.stderr.write(f"[plan] {title}: 실행 {len(rows)}회, 예상 {total / 60:.0f}분\n")
    for cid, why in excluded:
        sys.stderr.write(f"[plan]   제외 {cid}: {why}\n")
    for cid, why in notes:
        sys.stderr.write(f"[plan]   주의 {cid}: {why}\n")
    # 연결당 요청 간격이 서버 유휴 정리(90초)에 가까우면 연결이 끊겨 결과가 섞인다
    for r in rows:
        iv = r["meta"].get("CONN_INTERVAL_S")
        if iv is not None and float(iv) > 60:
            sys.stderr.write(f"[plan]   경고 {r['run_id']}: 연결당 요청 간격 {float(iv):.0f}s > 60s "
                             f"(서버 유휴 정리 90초) — 연결 수를 줄일 것\n")


def range_note(cal, r, cid, notes):
    """SIMPLE 도착률이 보정 표 범위 밖이면 처리 시간을 끝값으로 가정했다는 사실을 남긴다."""
    lam_s = (1 - r["p"]) * r["rate"]
    lo, hi = cal.simple_table[0][0], cal.simple_table[-1][0]
    if lam_s > 0 and not (lo <= lam_s <= hi):
        notes.append((cid, f"SIMPLE 도착률 {lam_s:.0f}/s 가 보정 범위 {lo:.0f}~{hi:.0f}/s 밖 — "
                           f"처리 시간을 끝값 {r['s_simple_us']:.2f}µs 로 가정"))


def reactor_ok(cal, peak):
    frac = envf("REACTOR_FRAC", 0.8)
    return peak <= frac * cal.reactor_limit, frac * cal.reactor_limit


# ── 실험 0 (보정) ────────────────────────────────────────────────────────

def plan_cal(part):
    warmup, duration = envf("CAL_WARMUP", 5), envf("CAL_DURATION", 20)
    cpu = env("CAL_WORKER_CPU", "1")
    seed = seed_for(1)
    rows = []
    if part == "0simple":
        conns = envi("CAL_CONNS", envi("LG_CONNS", 20000))
        for rate in envlist("CAL_SIMPLE_RATES",
                            "1000 2000 5000 10000 20000 30000 40000 50000 60000 80000"):
            rows.append({"run_id": f"simple_{rate}",
                         "server_env": f"SCALING=fixed WORKERS=1 WORKER_CPUS={cpu}",
                         "lg_args": f"{lg_common(conns, warmup, duration, seed)} --rate {rate}",
                         "expected_s": expected_seconds(conns, warmup, duration),
                         "meta": {"PART": "simple", "RATE": rate, "CONNS": conns,
                                  "CONN_INTERVAL_S": f"{conns / float(rate):.2f}"}})
    elif part == "0complex":
        conns = envi("CAL_COMPLEX_CONNS", 500)
        rate = env("CAL_COMPLEX_RATE", "30")
        rows.append({"run_id": "complex",
                     "server_env": f"SCALING=fixed WORKERS=1 WORKER_CPUS={cpu}",
                     "lg_args": (f"{lg_common(conns, warmup, duration, seed)} --rate {rate} "
                                 f"--complex-frac 1 --work-dist fixed"),
                     "expected_s": expected_seconds(conns, warmup, duration),
                     "meta": {"PART": "complex", "RATE": rate, "CONNS": conns,
                              "CONN_INTERVAL_S": f"{conns / float(rate):.2f}"}})
    elif part == "0reactor":
        conns = envi("LG_CONNS", 20000)
        for rate in envlist("CAL_REACTOR_RATES",
                            "20000 30000 40000 50000 60000 70000 80000 90000 100000 120000 140000"):
            rows.append({"run_id": f"reactor_{rate}",
                         "server_env": f"SCALING=fixed {env('CAL_REACTOR_ENV', '')}".strip(),
                         "lg_args": f"{lg_common(conns, warmup, duration, seed)} --rate {rate}",
                         "expected_s": expected_seconds(conns, warmup, duration),
                         "meta": {"PART": "reactor", "RATE": rate, "CONNS": conns,
                                  "CONN_INTERVAL_S": f"{conns / float(rate):.2f}"}})
    emit(rows, [], f"exp0 {part}")


# ── 실험 A: 지금 서버, 19조건 ────────────────────────────────────────────

def plan_a():
    cal = calib()
    conns, warmup, duration = envi("LG_CONNS", 20000), envf("WARMUP", 10), envf("DURATION", 75)
    period, width = envf("SYNC_PERIOD", 25), envf("SYNC_WIDTH", 3)
    srv = f"{env('A_SERVER_ENV', 'POOL_PROFILE=legacy WORKER_CPUS=1')} {mu0_env(cal)}"
    conds, excluded, notes = [], [], []
    for rho in envlist("A_RHOS", "0.5 0.8"):
        for share in envlist("A_SHARES", "0 0.5 0.9"):
            for f in envlist("A_SYNCS", "0 0.05 0.2 1.0"):
                r = rate_for(cal, float(rho), float(share), envf("WORK_US", 10000))
                peak = r["rate"] * peak_factor(float(f), period, width)
                cid = f"r{rho}_s{share}_f{f}"
                ok, lim = reactor_ok(cal, peak)
                if ok:
                    range_note(cal, r, cid, notes)
                if not ok:
                    excluded.append((cid, f"최대 유입 {peak:.0f}/s > 리액터 한계 × "
                                          f"{envf('REACTOR_FRAC', 0.8):g} = {lim:.0f}/s"))
                    continue
                conds.append((cid, rho, share, f, r, peak))
    rows = []
    for (cid, rho, share, f, r, peak), rep in order("A", conds, envi("REPS_A", 3)):
        rows.append({
            "run_id": f"{cid}_rep{rep}",
            "server_env": srv,
            "lg_args": (f"{lg_common(conns, warmup, duration, seed_for(rep))} "
                        f"--rate {r['rate']:.1f} --complex-frac {r['p']:.6f} --work-dist fixed "
                        f"--sync-frac {f}"),
            "expected_s": expected_seconds(conns, warmup, duration),
            "meta": {"COND": cid, "REP": rep, "RHO": rho, "SHARE": share, "SYNC": f,
                     "RATE": f"{r['rate']:.1f}", "P_COMPLEX": f"{r['p']:.6f}",
                     "S_SIMPLE_US": f"{r['s_simple_us']:.2f}", "ES_US": f"{r['es_us']:.2f}",
                     "PEAK_RATE": f"{peak:.0f}", "CONNS": conns,
                     "CONN_INTERVAL_S": f"{conns / r['rate']:.2f}"}})
    if cal.reactor_kind != "measured":
        sys.stderr.write("[plan] 주의: 리액터 한계가 하한값(생성기 한계)이라 제외가 보수적일 수 있음\n")
    emit(rows, excluded, "실험 A", notes)


# ── 실험 B: 같은 성능에서 비용 ───────────────────────────────────────────

B_DEFAULT_ENV = {
    "legacy": "POOL_PROFILE=legacy WORKER_CPUS=1",
    "dyncap1": "POOL_PROFILE=legacy CAP_MODE=dynamic WORKER_CPUS=1",
    "fixed3": "SCALING=fixed WORKERS=3",
    "adaptive": "",
}


def plan_b():
    cal = calib()
    conns, warmup, duration = envi("LG_CONNS", 20000), envf("WARMUP", 10), envf("DURATION", 75)
    period, width = envf("SYNC_PERIOD", 25), envf("SYNC_WIDTH", 3)
    rho = env("B_RHO", "0.8")
    conds, excluded, notes = [], [], []
    for load in envlist("B_LOADS", "0.9:0.2 0.9:1.0"):
        share, f = load.split(":")
        r = rate_for(cal, float(rho), float(share), envf("WORK_US", 10000))
        peak = r["rate"] * peak_factor(float(f), period, width)
        range_note(cal, r, f"s{share}_f{f}", notes)
        for name in envlist("B_CONFIGS", "legacy dyncap1 fixed3 adaptive"):
            cid = f"s{share}_f{f}_{name}"
            ok, lim = reactor_ok(cal, peak)
            if not ok:
                excluded.append((cid, f"최대 유입 {peak:.0f}/s > {lim:.0f}/s"))
                continue
            srv = env(f"B_ENV_{name}", B_DEFAULT_ENV.get(name, ""))
            conds.append((cid, name, share, f, r, peak, f"{srv} {mu0_env(cal)}".strip()))
    rows = []
    for (cid, name, share, f, r, peak, srv), rep in order("B", conds, envi("REPS_B", 5)):
        rows.append({
            "run_id": f"{cid}_rep{rep}",
            "server_env": srv,
            "lg_args": (f"{lg_common(conns, warmup, duration, seed_for(rep))} "
                        f"--rate {r['rate']:.1f} --complex-frac {r['p']:.6f} --work-dist fixed "
                        f"--sync-frac {f}"),
            "expected_s": expected_seconds(conns, warmup, duration),
            "meta": {"COND": cid, "REP": rep, "CONFIG": name, "RHO": rho, "SHARE": share,
                     "SYNC": f, "RATE": f"{r['rate']:.1f}", "P_COMPLEX": f"{r['p']:.6f}",
                     "PEAK_RATE": f"{peak:.0f}", "CONNS": conns,
                     "CONN_INTERVAL_S": f"{conns / r['rate']:.2f}"}})
    emit(rows, excluded, "실험 B", notes)


# ── 실험 C: Erlang C 검증 ────────────────────────────────────────────────

def plan_c():
    cal = calib()
    conns, warmup, duration = envi("C_CONNS", 500), envf("WARMUP", 10), envf("DURATION", 75)
    base = env("C_SERVER_ENV", "CAP_MODE=fixed QUEUE_CAP=1000000")
    conds = []
    for rate in envlist("C_RATES", "50 150 200"):
        for c in envlist("C_THREADS", "1 2 3"):
            conds.append((f"R{rate}_c{c}", rate, c))
    rows = []
    for (cid, rate, c), rep in order("C", conds, envi("REPS_C", 5)):
        rows.append({
            "run_id": f"{cid}_rep{rep}",
            "server_env": f"SCALING=fixed WORKERS={c} {base} {mu0_env(cal)}",
            "lg_args": (f"{lg_common(conns, warmup, duration, seed_for(rep))} --rate {rate} "
                        f"--complex-frac 1 --work-dist exp"),
            "expected_s": expected_seconds(conns, warmup, duration),
            "meta": {"COND": cid, "REP": rep, "RATE": rate, "THREADS": c, "CONNS": conns,
                     "CONN_INTERVAL_S": f"{conns / float(rate):.2f}"}})
    emit(rows, [], "실험 C")


# ── 실험 D: 외부 API 분리 ────────────────────────────────────────────────

def plan_d():
    cal = calib()
    conns = envi("D_CONNS", envi("LG_CONNS", 20000))
    warmup, duration = envf("D_WARMUP", 5), envf("D_DURATION", 130)
    rate = env("D_RATE", "4000")
    base = env("D_SERVER_ENV",
               "FAKE_API_MS=100 SCALING=fixed WORKERS=1 WORKER_CPUS=1 CAP_MODE=fixed QUEUE_CAP=10000")
    conds = [(m,) for m in envlist("D_ROUTERS", "sync async")]
    rows = []
    for (mode,), rep in order("D", conds, envi("REPS_D", 5)):
        rows.append({
            "run_id": f"{mode}_rep{rep}",
            "server_env": f"{base} ROUTER_MODE={mode} {mu0_env(cal)}",
            "lg_args": f"{lg_common(conns, warmup, duration, seed_for(rep))} --rate {rate}",
            "expected_s": expected_seconds(conns, warmup, duration),
            "meta": {"COND": mode, "REP": rep, "ROUTER": mode, "RATE": rate, "CONNS": conns,
                     "CONN_INTERVAL_S": f"{conns / float(rate):.2f}"}})
    emit(rows, [], "실험 D")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "runs":
        what = sys.argv[2]
        if what in ("0simple", "0complex", "0reactor"):
            plan_cal(what)
        elif what == "A":
            plan_a()
        elif what == "B":
            plan_b()
        elif what == "C":
            plan_c()
        elif what == "D":
            plan_d()
        else:
            die(f"알 수 없는 실험: {what}")
    elif len(sys.argv) == 4 and sys.argv[1] == "rate":
        r = rate_for(calib(), float(sys.argv[2]), float(sys.argv[3]), envf("WORK_US", 10000))
        print(" ".join(f"{k}={v:.4f}" for k, v in r.items()))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
