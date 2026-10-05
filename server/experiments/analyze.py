#!/usr/bin/env python3
"""실험 결과 정리.

사용:
  analyze.py calstep <run_dir> <simple|complex|reactor>   exp0 한 단계 판정 (셸 변수로 출력)
  analyze.py calwrite <results/exp0>                      보정 파일(calib.env) 내용 출력
  analyze.py report [A B C D]                              results/<exp>/{runs.csv,summary.csv,summary.md}
                                                          와 results/REPORT.md

반복 지표는 실행별 값(예: 실행마다의 p99)을 반복 평균하고 95% 신뢰구간(t 분포)을 붙인다.
여러 실행의 요청을 합쳐 다시 구한 백분위가 아니다.
"""
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from expcommon import (NAN, Calib, fmt, fmt_ci, fnum, isnum, load_run, load_runs, mean_ci,  # noqa: E402
                       min_servers_erlang, min_servers_naive, p_wait_gt, plan_ids, read_ts)

CLASSES = ("ALL", "SIMPLE", "COMPLEX")


def env(name, default):
    v = os.environ.get(name)
    return default if v is None or v == "" else v


def envf(name, default):
    return float(env(name, str(default)))


def results_dir():
    return env("RESULTS_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results"))


# ── 실행 1회 → 평평한 지표 ───────────────────────────────────────────────

SUMMARY_KEYS = ("sent", "ok", "shed", "late", "timeout", "skipped", "lost", "q_over",
                "lat_p50_ms", "lat_p99_ms", "lat_p999_ms", "lat_max_ms",
                "q_p50_ms", "q_p99_ms", "q_p999_ms", "q_max_ms", "s_p50_us", "s_p99_us",
                "rest_p50_ms", "rest_p99_ms", "gen_lag_p99_ms", "idc_10ms", "idc_100ms", "idc_1s")


def flatten(run):
    m = {}
    for cls in CLASSES:
        row = run["summary"].get(cls)
        if not row:
            continue
        sent = row.get("sent", NAN)
        for k in SUMMARY_KEYS:
            m[f"{cls}.{k}"] = fnum(row.get(k))
        if isnum(sent) and sent > 0:
            m[f"{cls}.shed_pct"] = 100.0 * (row["shed"] + row["late"]) / sent
            m[f"{cls}.sshed_pct"] = 100.0 * row["shed"] / sent      # 상한 초과로 안 받음(x=S)
            m[f"{cls}.late_pct"] = 100.0 * row["late"] / sent       # 받았지만 마감 초과(x=D)
            m[f"{cls}.timeout_pct"] = 100.0 * (row["timeout"] + row["lost"]) / sent
            m[f"{cls}.skip_pct"] = 100.0 * row["skipped"] / sent
            # 대기 목표(q > 임계) 위반 또는 정상 처리 실패 비율
            m[f"{cls}.p_bad"] = (row["q_over"] + row["shed"] + row["late"] + row["timeout"]
                                 + row["lost"]) / sent
    threads = cores = changes = 0.0
    have = False
    for pool, c in run["ctrl"].items():
        have = True
        for k, v in c.items():
            m[f"ctrl.{pool}.{k}"] = v
        threads += c["mean_c"]
        cores += c["cpu_cores"] if isnum(c["cpu_cores"]) else 0.0
        changes += c["changes_per_min"] if isnum(c["changes_per_min"]) else 0.0
        m["ctrl.reactor_util"] = c["reactor_util"]
        m["ctrl.proc_cores"] = c["proc_cores"]
    if have:
        m["ctrl.threads"] = threads   # 풀 합 (split 은 SIMPLE + COMPLEX)
        m["ctrl.worker_cores"] = cores
        m["ctrl.changes_per_min"] = changes
        m["ctrl.max_q"] = max(c["max_q"] for c in run["ctrl"].values())
    return m


def plan_key(meta):
    """같은 조건의 반복끼리 같아야 하는 설정 (시드는 반복마다 달라서 뺀다)."""
    lg = re.sub(r"--seed \S+", "", meta.get("LG_ARGS", ""))
    return (meta.get("SERVER_ENV", ""), " ".join(lg.split()), meta.get("RATE", ""))


def group(runs):
    """COND 별로 묶는다 → {cond: {"meta", "runs": [평평한 지표], "raw", "discarded", "mismatch"}}
    생성기 송신 지연 p99 > GENLAG_DISCARD_MS 인 실행은 생성기 포화로 보고 평균에서 뺀다."""
    limit = envf("GENLAG_DISCARD_MS", 10)
    g = {}
    for r in runs:
        cond = r["meta"].get("COND", r["meta"].get("RUN_ID"))
        e = g.setdefault(cond, {"meta": r["meta"], "runs": [], "raw": [], "discarded": [],
                                "mismatch": []})
        fl = flatten(r)
        if fl.get("ALL.gen_lag_p99_ms", 0) > limit:
            e["discarded"].append(f"{r['meta'].get('RUN_ID')} (생성기 지연 p99 "
                                  f"{fl['ALL.gen_lag_p99_ms']:.1f}ms)")
            continue
        if e["raw"] and plan_key(r["meta"]) != plan_key(e["raw"][0]["meta"]):
            e["mismatch"].append(r["meta"].get("RUN_ID"))
        e["runs"].append(fl)
        e["raw"].append(r)
    return g   # 실행이 모두 빠진 조건도 남겨 n=0 으로 보이게 한다


def agg(entry, key):
    return mean_ci([fr.get(key, NAN) for fr in entry["runs"]])


def write_runs_csv(path, runs):
    flats = [(r["meta"], flatten(r)) for r in runs]
    meta_keys = sorted({k for m, _ in flats for k in m})
    met_keys = sorted({k for _, f in flats for k in f})
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(meta_keys + met_keys)
        for m, fl in flats:
            w.writerow([m.get(k, "") for k in meta_keys] + [fl.get(k, "") for k in met_keys])


def write_summary_csv(path, groups, keys):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["cond", "n"] + [x for k in keys for x in (f"{k}.mean", f"{k}.ci95")])
        for cond, e in groups.items():
            row = [cond, len(e["runs"])]
            for k in keys:
                mn, h, _ = agg(e, k)
                row += [mn, h]
            w.writerow(row)


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def failed_runs(exp_dir):
    out = []
    if os.path.isdir(exp_dir):
        for name in sorted(os.listdir(exp_dir)):
            p = os.path.join(exp_dir, name, "FAILED")
            if os.path.exists(p):
                out.append(f"{name}: {open(p, encoding='utf-8').read().strip()}")
    return out


def plan_notes(exp_dir):
    p = os.path.join(exp_dir, "plan.log")
    if not os.path.exists(p):
        return []
    return [line.strip() for line in open(p, encoding="utf-8") if "제외" in line or "경고" in line
            or "주의" in line]


def common_footer(exp_dir, groups, extra=()):
    lines = []
    for cond, e in groups.items():
        for d in e.get("discarded", []):
            lines.append(f"- 평균에서 뺀 실행(생성기 포화): {cond} / {d}")
        if e.get("mismatch"):
            lines.append(f"- 경고: {cond} 의 반복끼리 설정이 다름(서버 env·생성기 인자·도착률): "
                         f"{', '.join(e['mismatch'])} — 다른 설정의 결과가 함께 평균됨")
    if extra:
        lines.append(f"- 지금 계획에 없는 예전 완료 실행 {len(extra)}개는 보고서에서 뺐음: "
                     f"{', '.join(extra[:10])}{' …' if len(extra) > 10 else ''}")
    skip = [fr.get("ALL.skip_pct", 0) for e in groups.values() for fr in e["runs"]]
    if any(isnum(x) and x > 0 for x in skip):
        lines.append("- 경고: 생성기가 빈 연결을 못 찾아 보내지 못한 요청이 있음(skip%) — 연결 수·동시 요청 수를 늘릴 것")
    for n in plan_notes(exp_dir):
        lines.append(f"- 계획: {n}")
    for n in failed_runs(exp_dir):
        lines.append(f"- 실패한 실행: {n}")
    return lines


# ── 실험 0 ───────────────────────────────────────────────────────────────

def calstep(run_dir, part):
    r = load_run(run_dir)
    prefix = {"simple": "CS", "complex": "CC", "reactor": "CR"}[part]
    if r is None:
        print(f"{prefix}_DONE=0")
        return
    fl = flatten(r)
    print(f"{prefix}_DONE=1")
    ctrl = r["ctrl"]
    simple = ctrl.get("SIMPLE") or ctrl.get("SHARED") or {}
    span = simple.get("span_s", NAN)
    if part == "simple":
        rate = simple.get("offered", NAN) / span if isnum(span) and span > 0 else NAN
        s_us = simple.get("svc_us", NAN)
        util = rate * s_us * 1e-6 if isnum(rate) and isnum(s_us) else NAN
        stop = (not isnum(util)) or util >= envf("CAL_SIMPLE_MAX_UTIL", 0.85) \
            or fl.get("ALL.shed_pct", 0) > 0 or fl.get("ALL.gen_lag_p99_ms", 0) > envf("CAL_GENLAG_P99_MS", 2)
        print(f"CS_RATE={rate:.1f}\nCS_S_US={s_us:.3f}\nCS_UTIL={util:.4f}\nCS_STOP={int(stop)}")
    elif part == "complex":
        cplx = ctrl.get("COMPLEX", {})
        print(f"CC_S_US={cplx.get('svc_us', NAN):.3f}")
    else:
        ok_frac = fl["ALL.ok"] / fl["ALL.sent"] if fl.get("ALL.sent") else 0.0
        rest = fl.get("ALL.rest_p99_ms", NAN)
        genlag = fl.get("ALL.gen_lag_p99_ms", NAN)
        genlimit = isnum(genlag) and genlag > envf("CAL_GENLAG_P99_MS", 2)
        ok = ok_frac >= envf("CAL_REACTOR_OK_FRAC", 0.999) and isnum(rest) \
            and rest <= envf("CAL_REST_P99_MS", 5) and not genlimit
        util = fl.get("ctrl.reactor_util", NAN)
        print(f"CR_PASS={int(ok)}\nCR_GENLIMIT={int(genlimit)}\nCR_OKFRAC={ok_frac:.5f}\n"
              f"CR_REST_P99={rest:.3f}\nCR_UTIL={util:.4f}")


def calwrite(exp0_dir):
    sess = os.path.join(exp0_dir, "session_runs.txt")
    only = None
    if os.path.exists(sess):
        only = {line.strip() for line in open(sess, encoding="utf-8") if line.strip()}
    runs = load_runs(exp0_dir, only)
    simple, reactor, complex_s = [], [], None
    for r in runs:
        part = r["meta"].get("PART")
        fl = flatten(r)
        c = r["ctrl"].get("SIMPLE", {})
        if part == "simple":
            span = c.get("span_s", NAN)
            rate = c.get("offered", NAN) / span if isnum(span) and span > 0 else NAN
            util = rate * c.get("svc_us", NAN) * 1e-6
            # 포화(큐가 쌓임)나 shed 가 있는 실행은 처리 시간이 대기에 오염되지 않았어도 제외한다
            if isnum(util) and util < 0.95 and fl.get("ALL.shed_pct", 0) == 0:
                simple.append((rate, c["svc_us"]))
        elif part == "complex":
            complex_s = r["ctrl"].get("COMPLEX", {}).get("svc_us", NAN)
        elif part == "reactor":
            ok_frac = fl["ALL.ok"] / fl["ALL.sent"] if fl.get("ALL.sent") else 0.0
            genlag = fl.get("ALL.gen_lag_p99_ms", NAN)
            genlimit = isnum(genlag) and genlag > envf("CAL_GENLAG_P99_MS", 2)
            ok = ok_frac >= envf("CAL_REACTOR_OK_FRAC", 0.999) \
                and fl.get("ALL.rest_p99_ms", NAN) <= envf("CAL_REST_P99_MS", 5) and not genlimit
            reactor.append((fnum(r["meta"].get("RATE")), ok, genlimit,
                            fl.get("ctrl.reactor_util", NAN), fl.get("ALL.rest_p99_ms", NAN)))
    if not simple:
        sys.exit("[analyze] SIMPLE 보정 결과 없음")
    if not isnum(complex_s if complex_s is not None else NAN):
        sys.exit("[analyze] COMPLEX 보정 결과 없음")
    if not reactor:
        sys.exit("[analyze] 리액터 보정 결과 없음")
    simple.sort()
    reactor.sort()
    limit, kind, util_at = NAN, "lower_bound", NAN
    for rate, ok, genlimit, util, _ in reactor:
        if ok:
            limit, util_at = rate, util
            continue
        kind = "lower_bound" if genlimit else "measured"
        break
    if not isnum(limit):
        sys.exit("[analyze] 가장 낮은 리액터 보정 도착률부터 실패 — CAL_REACTOR_RATES 를 낮출 것")
    extrap = limit / util_at if isnum(util_at) and util_at > 0 else NAN
    work = envf("WORK_US", 10000)
    print("# exp0 보정 결과 — analyze.py calwrite 가 생성. 손으로 고쳐도 됨")
    print("# SIMPLE: (실측 도착률:처리 시간 µs), 워커 1코어, 스레드 점유 시간(송신·close 포함)")
    print(f'CAL_SIMPLE_TABLE="{" ".join(f"{r:.0f}:{s:.2f}" for r, s in simple)}"')
    print(f"CAL_S_COMPLEX_US={complex_s:.1f}")
    print(f"CAL_WORK_US={work:g}")
    print("# 리액터 한계: 통과 = 정상 응답 ≥ 99.9%, 응답 지연−대기−처리 p99 ≤ "
          f"{envf('CAL_REST_P99_MS', 5):g}ms, 생성기 지연 p99 ≤ {envf('CAL_GENLAG_P99_MS', 2):g}ms")
    print("#   measured = 다음 단계에서 서버 쪽이 실패, lower_bound = 생성기 한계 또는 시험한 최대값까지 통과")
    for rate, ok, genlimit, util, rest in reactor:
        print(f"#   {rate:.0f}/s: {'통과' if ok else '실패'}{' (생성기 한계)' if genlimit else ''} "
              f"리액터 사용률 {fmt(util, 3)} rest_p99 {fmt(rest)}ms")
    print(f"CAL_REACTOR_LIMIT={limit:.0f}")
    print(f"CAL_REACTOR_LIMIT_KIND={kind}")
    print(f"CAL_REACTOR_UTIL_AT_LIMIT={fmt(util_at, 4)}")
    print(f"CAL_REACTOR_LIMIT_EXTRAP={fmt(extrap, 0)}   # 한계 도착률 ÷ 그때 리액터 사용률 (선형 가정)")
    print("# 서버 μ 초기값: 처리 표본이 생기기 전 첫 10ms 동안만 쓰인다. 낮은 도착률 값(보수적)")
    print(f"MU0_SIMPLE={1e6 / simple[0][1]:.0f}")
    print(f"MU0_COMPLEX={1e6 / complex_s:.2f}")


# ── 실험 A ───────────────────────────────────────────────────────────────

def report_a(exp_dir, groups, extra=()):
    keys = ["ALL.lat_p99_ms", "ALL.q_p99_ms", "ALL.q_max_ms", "ALL.sshed_pct", "ALL.late_pct",
            "ALL.timeout_pct", "ctrl.max_q", "ctrl.worker_cores", "ALL.idc_1s"]
    write_summary_csv(os.path.join(exp_dir, "summary.csv"), groups, keys)
    rows = []
    order = sorted(groups.items(), key=lambda kv: (fnum(kv[1]["meta"].get("RHO")),
                                                   fnum(kv[1]["meta"].get("SHARE")),
                                                   fnum(kv[1]["meta"].get("SYNC"))))
    for cond, e in order:
        m = e["meta"]
        senv = m.get("SERVER_ENV", "")
        cap = fnum(next((t.split("=")[1] for t in senv.split() if t.startswith("QUEUE_CAP=")),
                        "10000"))
        es = fnum(m.get("ES_US"))
        k_wait = cap * es * 1e-6   # 상한 K 를 1코어 처리 속도로 비우는 시간
        lat, lath, _ = agg(e, "ALL.lat_p99_ms")
        q, qh, _ = agg(e, "ALL.q_p99_ms")
        qmax, _, _ = agg(e, "ALL.q_max_ms")
        sshed, sshedh, _ = agg(e, "ALL.sshed_pct")
        late, _, _ = agg(e, "ALL.late_pct")
        tmo, _, _ = agg(e, "ALL.timeout_pct")
        mq, _, _ = agg(e, "ctrl.max_q")
        cores, _, _ = agg(e, "ctrl.worker_cores")
        labels = []
        if isnum(sshed) and sshed > 0.1:
            labels.append("부족(상한 도달)")
        if (isnum(late) and late > 0.1) or (isnum(tmo) and tmo > 0.1) or (isnum(qmax) and qmax > 1000):
            labels.append("과다(대기 > 1초)")
        verdict = " + ".join(labels) if labels else "여유"
        if not e["runs"]:
            verdict = "-"
        rows.append([m.get("RHO"), fmt(cores, 2), m.get("SHARE"), m.get("SYNC"),
                     fmt(fnum(m.get("RATE")), 0), fmt(fnum(m.get("PEAK_RATE")), 0), fmt(k_wait, 2),
                     fmt_ci(lat, lath, 1), fmt_ci(q, qh, 1), fmt(qmax, 0), fmt_ci(sshed, sshedh, 2),
                     fmt(late, 2), fmt(tmo, 2), fmt(mq, 0), verdict, len(e["runs"])])
    md = ["# 실험 A — 지금 서버(고정 큐 10,000)",
          "",
          "- ρ(계획) = 보정(v5 split 풀, 스레드 1개)으로 잡은 1코어 기준 이용률. 실측 워커 CPU = 이 서버"
          "(공용 큐, 스레드 16개)의 워커 스레드가 실제로 쓴 코어 수. 둘이 크게 다르면 ρ 표시는 명목값으로 읽는다.",
          "- K 대기(초) = 상한 × 평균 처리 시간(1코어): 큐가 꽉 찼을 때 마지막 순번의 대기.",
          "- shed = 상한 초과로 안 받음(x=S), late = 받았지만 마감 초과(x=D, legacy 는 마감 없음), "
          "timeout = 3초 안에 응답 없음. q 는 응답을 받은 요청의 서버 큐 대기(late 포함, timeout 제외).",
          "- 판정: shed > 0.1% → 부족(상한 도달) / late 또는 timeout > 0.1%, 또는 q 최대 > 1초 → "
          "과다(대기 > 1초). 둘 다면 둘 다 적고, 둘 다 아니면 여유.",
          "",
          md_table(["ρ(계획)", "실측 워커 CPU", "COMPLEX CPU", "몰림", "λ(/s)", "최대 유입(/s)",
                    "K 대기(s)", "응답 p99(ms)", "q p99(ms)", "q 최대(ms)", "shed(%)", "late(%)",
                    "timeout(%)", "큐 최대(개)", "판정", "n"], rows), ""]
    md += common_footer(exp_dir, groups, extra)
    return "\n".join(md)


# ── 실험 B ───────────────────────────────────────────────────────────────

# 설계 문서의 시뮬레이션 예측 (이용률 0.8): p99 대기(ms) · shed(%) · 평균 활성 스레드
B_PRED = {
    ("0.5", "0.2"): {"legacy": (589, 11.7, 1), "dyncap1": (990, 9.7, 1), "fixed3": (0, 0, 3),
                     "adaptive": (9, 0, 1.27)},
    ("0.9", "0.2"): {"legacy": (2834, 0.2, 1), "dyncap1": (990, 9.6, 1), "fixed3": (0, 0, 3),
                     "adaptive": (10, 0, 2.06)},
    ("0.9", "1.0"): {"legacy": (2882, 70.6, 1), "dyncap1": (990, 80.1, 1), "fixed3": (990, 40.2, 3),
                     "adaptive": (990, 40.3, 1.40)},
}


def norm(x):
    v = fnum(x)
    return f"{v:.1f}" if isnum(v) else str(x)


def report_b(exp_dir, groups, extra=()):
    keys = ["ALL.q_p99_ms", "ALL.lat_p99_ms", "SIMPLE.q_p99_ms", "COMPLEX.q_p99_ms",
            "ALL.sshed_pct", "ALL.late_pct", "ALL.timeout_pct", "ctrl.SIMPLE.mean_c",
            "ctrl.COMPLEX.mean_c", "ctrl.SHARED.mean_c", "ctrl.worker_cores",
            "ctrl.changes_per_min", "ctrl.reactor_util"]
    write_summary_csv(os.path.join(exp_dir, "summary.csv"), groups, keys)
    cfg_order = env("B_CONFIGS", "legacy dyncap1 fixed3 adaptive").split()
    rows = []
    order = sorted(groups.items(), key=lambda kv: (fnum(kv[1]["meta"].get("SHARE")),
                                                   fnum(kv[1]["meta"].get("SYNC")),
                                                   cfg_order.index(kv[1]["meta"].get("CONFIG"))
                                                   if kv[1]["meta"].get("CONFIG") in cfg_order else 99))
    for cond, e in order:
        m = e["meta"]
        cfg = m.get("CONFIG")
        pred = B_PRED.get((norm(m.get("SHARE")), norm(m.get("SYNC"))), {}).get(cfg)
        q, qh, _ = agg(e, "ALL.q_p99_ms")
        lat, _, _ = agg(e, "ALL.lat_p99_ms")
        qs, _, _ = agg(e, "SIMPLE.q_p99_ms")
        qc, _, _ = agg(e, "COMPLEX.q_p99_ms")
        sshed, sshedh, _ = agg(e, "ALL.sshed_pct")
        late, _, _ = agg(e, "ALL.late_pct")
        tmo, _, _ = agg(e, "ALL.timeout_pct")
        cs, _, _ = agg(e, "ctrl.SIMPLE.mean_c")
        cc, _, _ = agg(e, "ctrl.COMPLEX.mean_c")
        csh, _, _ = agg(e, "ctrl.SHARED.mean_c")
        threads = f"공용 {fmt(csh, 1)}" if isnum(csh) else f"{fmt(cs, 2)} / {fmt(cc, 2)}"
        cores, coresh, _ = agg(e, "ctrl.worker_cores")
        ch, _, _ = agg(e, "ctrl.changes_per_min")
        rows.append([m.get("SHARE"), m.get("SYNC"), cfg, fmt_ci(q, qh, 1), fmt(lat, 1), fmt(qs, 1),
                     fmt(qc, 1), fmt_ci(sshed, sshedh, 2), fmt(late, 2), fmt(tmo, 2), threads,
                     fmt_ci(cores, coresh, 2), fmt(ch, 1),
                     f"{pred[0]} · {pred[1]} · {pred[2]}" if pred else "-", len(e["runs"])])
    md = ["# 실험 B — 같은 부하에서 설정별 비용",
          "",
          "- q = 서버 큐 대기(late 포함, timeout 제외). 응답 p99 = 예정 송신 → 수신 (legacy 처럼 한 코어에 "
          "스레드가 여럿이면 CPU 를 기다리는 시간이 q 가 아니라 처리 시간에 들어가므로 함께 본다).",
          "- 활성 스레드 = 종류별 평균 c (SIMPLE / COMPLEX). fixed3 은 종류마다 3개. legacy·dyncap1 은 "
          "공용 풀 하나. 비용 비교의 기준은 워커 CPU(워커 스레드가 쓴 CPU 시간 ÷ 측정 시간, 코어 수)다.",
          "- 설계 예측 = 설계 문서의 유체 시뮬레이션(처리시간 분산 대기 제외): p99 대기(ms) · shed(%) · "
          "평균 활성 서버 수. 시뮬레이션은 종류를 나누지 않은 서버 풀 하나라, 세 번째 값은 위의 종류별 "
          "활성 스레드와 같은 양이 아니다(워커 CPU 와 비교).",
          "",
          md_table(["COMPLEX CPU", "몰림", "설정", "q p99 전체(ms)", "응답 p99 전체(ms)", "q p99 SIMPLE",
                    "q p99 COMPLEX", "shed(%)", "late(%)", "timeout(%)", "활성 스레드",
                    "워커 CPU(코어)", "조정/분", "설계 예측", "n"], rows), ""]
    md += common_footer(exp_dir, groups, extra)
    return "\n".join(md)


# ── 실험 C ───────────────────────────────────────────────────────────────

def report_c(exp_dir, groups, extra=()):
    keys = ["COMPLEX.p_bad", "COMPLEX.q_p99_ms", "COMPLEX.shed_pct", "COMPLEX.timeout_pct",
            "ctrl.COMPLEX.mu_median", "ctrl.COMPLEX.svc_us"]
    write_summary_csv(os.path.join(exp_dir, "summary.csv"), groups, keys)
    try:
        cal = Calib(env("CALIB_FILE", os.path.join(results_dir(), "calib.env")))
        mu = 1e6 / cal.s_complex(envf("WORK_US", 10000))
    except (FileNotFoundError, KeyError, ValueError):
        cal, mu = None, NAN
    t = envf("Q_THRESHOLD_MS", 50) / 1000.0
    pmax = 0.01
    by_rate = {}
    for cond, e in groups.items():
        by_rate.setdefault(fnum(e["meta"].get("RATE")), []).append(e)
    rows, verdict = [], []
    for rate in sorted(by_rate):
        observed_min = None
        for e in sorted(by_rate[rate], key=lambda x: fnum(x["meta"].get("THREADS"))):
            c = int(fnum(e["meta"].get("THREADS")))
            p, ph, n = agg(e, "COMPLEX.p_bad")
            mu_m, _, _ = agg(e, "ctrl.COMPLEX.mu_median")
            pred = p_wait_gt(c, rate, mu, t) if isnum(mu) else NAN
            rho = rate / (c * mu) if isnum(mu) else NAN
            if observed_min is None and isnum(p) and p <= pmax:
                observed_min = c
            rows.append([fmt(rate, 0), c, fmt(rho, 3), fmt_ci(p * 100, ph * 100 if isnum(ph) else NAN, 3),
                         fmt(pred * 100, 3), fmt(mu_m, 1), n])
        em = min_servers_erlang(rate, mu, t, pmax, 0.9) if isnum(mu) else None
        nv = min_servers_naive(rate, mu, 0.9) if isnum(mu) else None
        tested_max = max(int(fnum(x["meta"].get("THREADS"))) for x in by_rate[rate])
        if em is None or nv is None:
            which = "판정 불가(보정 없음)"
        elif observed_min is None:
            # 시험한 c 로는 기준을 못 맞춤: 두 예측이 모두 시험 범위 위면 판정 불가
            which = ("Erlang C" if em > tested_max >= nv else
                     "판정 불가(둘 다 시험 범위 밖)" if em > tested_max and nv > tested_max else
                     "어느 쪽도 아님")
        elif em == nv:
            which = "둘 다" if observed_min == em else "어느 쪽도 아님"
        else:
            which = ("Erlang C" if observed_min == em else
                     "단순 계산" if observed_min == nv else "어느 쪽도 아님")
        verdict.append([fmt(rate, 0), observed_min if observed_min else "> 시험 범위",
                        em or "-", nv or "-", which])
    md = ["# 실험 C — Erlang C 검증 (COMPLEX 만, 처리 시간 지수분포)",
          "",
          f"P(나쁨) = (q > {t * 1000:g}ms + shed + late + timeout) ÷ 보낸 요청. 기준 1%. "
          f"예측 = M/M/c 정확식 C(c,a)·exp(−(cμ−λ)t), μ = 1/보정 처리시간 = {fmt(mu, 1)}/s.",
          "단순 계산 = ⌈λ / (0.9μ)⌉ (이용률 0.9 이하만 요구).",
          "",
          md_table(["λ(/s)", "c", "ρ", "P(나쁨) 실측(%)", "P(Wq>t) 예측(%)", "μ̂ 실측(/s)", "n"], rows),
          "",
          md_table(["λ(/s)", "실측 최소 c (평균 ≤ 1%)", "Erlang C 최소 c", "단순 계산 최소 c", "실측과 맞는 쪽"],
                   verdict), ""]
    md += common_footer(exp_dir, groups, extra)
    return "\n".join(md)


# ── 실험 D ───────────────────────────────────────────────────────────────

def spike_stats(raw, thresh_ms):
    """측정 구간 100ms 칸 중 SIMPLE 최대 지연 > 임계인 칸 수와 그 시각."""
    ts = read_ts(os.path.join(raw["dir"], "lg_ts.csv"))
    s = raw["summary"]["ALL"]
    w0, w1 = s.get("warmup_s", 0), s.get("warmup_s", 0) + s.get("duration_s", 0)
    hits = [r["t_s"] for r in ts if w0 <= r["t_s"] < w1 and r.get("lat_max_simple_ms", 0) > thresh_ms]
    return len(hits), hits


def report_d(exp_dir, groups, extra=()):
    keys = ["SIMPLE.lat_p99_ms", "SIMPLE.lat_p999_ms", "SIMPLE.lat_max_ms", "SIMPLE.q_p999_ms",
            "SIMPLE.shed_pct", "SIMPLE.timeout_pct"]
    write_summary_csv(os.path.join(exp_dir, "summary.csv"), groups, keys)
    thresh = envf("D_SPIKE_MS", 50)
    rows, times = [], []
    for cond, e in sorted(groups.items()):
        spikes = [spike_stats(r, thresh) for r in e["raw"]]
        sp, sph, _ = mean_ci([float(n) for n, _ in spikes])
        p99, p99h, _ = agg(e, "SIMPLE.lat_p99_ms")
        p999, p999h, _ = agg(e, "SIMPLE.lat_p999_ms")
        mx, mxh, _ = agg(e, "SIMPLE.lat_max_ms")
        shed, _, _ = agg(e, "SIMPLE.shed_pct")
        rows.append([cond, fmt_ci(p99, p99h, 2), fmt_ci(p999, p999h, 1), fmt_ci(mx, mxh, 0),
                     fmt(shed, 3), fmt_ci(sp, sph, 1), len(e["runs"])])
        first = spikes[0][1] if spikes else []
        times.append(f"- {cond} 첫 실행의 튐 시각(생성기 시작 기준 초): "
                     f"{', '.join(f'{x:.1f}' for x in first[:20]) or '없음'}")
    md = ["# 실험 D — 외부 API 를 조회 경로에서 분리 (가짜 API, 워커 1)",
          "",
          f"튐 = 측정 구간 100ms 칸 중 SIMPLE 최대 응답 지연 > {thresh:g}ms 인 칸 수. 캐시 TTL 60초라 "
          "갱신은 약 60초 간격으로 일어난다.",
          "",
          md_table(["라우터", "p99(ms)", "p99.9(ms)", "최대(ms)", "shed+late(%)", "튐(칸)", "n"], rows),
          ""] + times + [""]
    md += common_footer(exp_dir, groups, extra)
    return "\n".join(md)


def report_0(exp_dir):
    p = env("CALIB_FILE", os.path.join(results_dir(), "calib.env"))
    if not os.path.exists(p):
        return "# 실험 0 — 보정 파일 없음"
    return "# 실험 0 — 보정 결과\n\n```\n" + open(p, encoding="utf-8").read() + "```\n"


def report(exps):
    """요청한 실험의 runs.csv·summary.csv·summary.md 를 다시 만들고, REPORT.md 는 summary.md 가
    있는 모든 실험(0, A~D)으로 다시 엮는다(한 실험만 돌려도 다른 실험 절이 사라지지 않게)."""
    root = results_dir()
    fns = {"A": report_a, "B": report_b, "C": report_c, "D": report_d}
    for exp in exps:
        d = os.path.join(root, exp)
        runs = load_runs(d, plan_ids(d))
        extra = list(load_runs.extra)
        if not runs:
            sys.stderr.write(f"[analyze] {exp}: 완료된 실행 없음\n")
            continue
        write_runs_csv(os.path.join(d, "runs.csv"), runs)
        md = fns[exp](d, group(runs), extra)
        with open(os.path.join(d, "summary.md"), "w", encoding="utf-8") as f:
            f.write(md + "\n")
        sys.stderr.write(f"[analyze] {exp}: 실행 {len(runs)}개 → {d}/summary.md\n")
    parts = []
    if os.path.exists(env("CALIB_FILE", os.path.join(root, "calib.env"))):
        parts.append(report_0(os.path.join(root, "exp0")))
    for exp in ("A", "B", "C", "D"):
        p = os.path.join(root, exp, "summary.md")
        if os.path.exists(p):
            parts.append(open(p, encoding="utf-8").read().rstrip())
    with open(os.path.join(root, "REPORT.md"), "w", encoding="utf-8") as f:
        f.write("\n\n".join(parts) + "\n")
    sys.stderr.write(f"[analyze] → {root}/REPORT.md\n")


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "calstep":
        calstep(a[1], a[2])
    elif len(a) == 2 and a[0] == "calwrite":
        calwrite(a[1])
    elif a and a[0] == "report":
        report(a[1:] or ["A", "B", "C", "D"])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
