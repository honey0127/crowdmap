#!/usr/bin/env bash
# 실험 B: {지금 서버, 동적 상한 1코어, 3코어 고정, 적응 1~3} × 부하 × 5회 — 같은 부하에서 비용 비교.
# 설정은 exp.conf (exp.conf.example 참고). DRY_RUN=1 이면 실행 목록과 예상 시간만 출력.
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
load_conf
run_experiment B
