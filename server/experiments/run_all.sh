#!/usr/bin/env bash
# 실험 0 → A → B → C → D → 보고서. 보정 파일이 있으면 실험 0 은 건너뛴다.
# 보정을 다시 재려면 results/calib.env 와 results/exp0 를 함께 지운다(calib.env 만 지우면
# 바이너리·설정이 같은 단계는 예전 측정값으로 판정만 다시 한다).
# 실험 하나가 실패해도 다음 실험으로 넘어간다. 완료된 실행은 다시 돌리지 않는다(이어하기).
# 사용: ./run_all.sh [A B C D]    (인자를 주면 그 실험만)
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
load_conf
exps=("$@")
[ ${#exps[@]} -eq 0 ] && exps=(A B C D)

if [ ! -f "$CALIB_FILE" ]; then
    "$EXP_DIR/exp0_calibrate.sh" || die "보정 실패"
    [ "$DRY_RUN" = "1" ] && { log "DRY_RUN: 보정 파일이 없어 A~D 계획은 만들 수 없음"; exit 0; }
fi

failed=()
for e in "${exps[@]}"; do
    "$EXP_DIR/exp$e.sh" || failed+=("$e")
done
[ "$DRY_RUN" = "1" ] && exit 0
python3 "$EXP_DIR/analyze.py" report "${exps[@]}"
if [ ${#failed[@]} -gt 0 ]; then
    log "실패한 실행이 있는 실험: ${failed[*]} — results/<실험>/*/FAILED 확인 후 같은 명령으로 이어하기"
    exit 1
fi
log "끝: $RESULTS_DIR/REPORT.md"
