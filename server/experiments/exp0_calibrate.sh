#!/usr/bin/env bash
# 실험 0: 보정 → $CALIB_FILE (기본 results/calib.env)
#   1) SIMPLE 처리 시간: 워커 1코어(CAL_WORKER_CPU), 도착률을 올려 가며 잰다. 처리 시간이 도착률에
#      따라 변하므로(깨우기 비용 상각) 한 값이 아니라 (도착률, 처리 시간) 표로 남긴다.
#      이용률이 CAL_SIMPLE_MAX_UTIL(0.85)에 닿거나 shed·생성기 포화가 생기면 멈춘다.
#   2) COMPLEX 처리 시간: 낮은 도착률(CAL_COMPLEX_RATE)에서 고정 작업량.
#   3) 리액터 한계: 워커 3코어, SIMPLE 만, 도착률을 올려 가며 처음 실패하기 직전 값.
#      통과 = 정상 응답 ≥ 99.9%, (응답 지연 − 큐 대기 − 처리) p99 ≤ CAL_REST_P99_MS(5ms),
#      생성기 송신 지연 p99 ≤ CAL_GENLAG_P99_MS(2ms). 생성기가 먼저 포화되면 하한값으로 기록.
# 이어하기: 완료된 단계(DONE)는 다시 돌리지 않고 판정만 다시 한다.
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
load_conf
OUT="$RESULTS_DIR/exp0"
mkdir -p "$OUT"
: > "$OUT/plan.log"

sweep() {   # <plan 대상> <calstep 종류>
    local target=$1 kind=$2 plan="$OUT/plan_$1.txt"
    make_plan "$target" "$plan" "$OUT/plan_$1.log"
    cat "$OUT/plan_$1.log" >> "$OUT/plan.log"
    if [ "$DRY_RUN" = "1" ]; then
        run_plan exp0 "$plan"
        return 0
    fi
    local id senv lgargs expect meta
    while IFS='|' read -r -u 3 id senv lgargs expect meta; do
        log "[exp0 $kind] $id"
        if ! run_one exp0 "$id" "$senv" "$lgargs" "$expect" "$meta"; then
            log "  실패 — 이 단계의 남은 도착률은 건너뜀"
            break
        fi
        eval "$(python3 "$EXP_DIR/analyze.py" calstep "$OUT/$id" "$kind")"
        case $kind in
            simple)
                log "  실측 ${CS_RATE}/s  S=${CS_S_US}µs  이용률=${CS_UTIL}"
                [ "${CS_STOP:-1}" = "1" ] && { log "  이용률 상한·shed·생성기 포화 → SIMPLE 보정 끝"; break; }
                ;;
            complex)
                log "  S=${CC_S_US}µs"
                ;;
            reactor)
                log "  통과=${CR_PASS} 정상 응답=${CR_OKFRAC} rest_p99=${CR_REST_P99}ms 리액터 사용률=${CR_UTIL}"
                [ "${CR_PASS:-0}" = "1" ] || {
                    [ "${CR_GENLIMIT:-0}" = "1" ] && log "  생성기 포화 → 리액터 한계는 하한값"
                    break
                }
                ;;
        esac
    done 3< "$plan"
}

[ "$DRY_RUN" = "1" ] || preflight exp0
sweep 0simple simple
sweep 0complex complex
sweep 0reactor reactor
[ "$DRY_RUN" = "1" ] && exit 0

if python3 "$EXP_DIR/analyze.py" calwrite "$OUT" > "$CALIB_FILE.tmp"; then
    mv "$CALIB_FILE.tmp" "$CALIB_FILE"
    log "보정 파일: $CALIB_FILE"
    cat "$CALIB_FILE" >&2
else
    rm -f "$CALIB_FILE.tmp"
    die "보정 실패 — $OUT 아래 FAILED·loadgen.log 확인"
fi
