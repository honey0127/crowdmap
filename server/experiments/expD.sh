#!/usr/bin/env bash
# 실험 D: 가짜 API(100ms), 라우터 sync/async × 5회 — 워커 1의 갱신 지연 튐이 사라지는지.
# 설정은 exp.conf (exp.conf.example 참고). DRY_RUN=1 이면 실행 목록과 예상 시간만 출력.
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
load_conf
run_experiment D
