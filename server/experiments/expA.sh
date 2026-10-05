#!/usr/bin/env bash
# 실험 A: 지금 서버(POOL_PROFILE=legacy, 워커 1코어)로 이용률 × COMPLEX 비중 × 몰림 19조건 × 3회.
# 설정은 exp.conf (exp.conf.example 참고). DRY_RUN=1 이면 실행 목록과 예상 시간만 출력.
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
load_conf
run_experiment A
