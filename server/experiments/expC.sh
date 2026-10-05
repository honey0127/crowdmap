#!/usr/bin/env bash
# 실험 C: COMPLEX 만(지수분포), 도착률 × 고정 스레드 수 × 5회 — Erlang C 최소 스레드 검증.
# 설정은 exp.conf (exp.conf.example 참고). DRY_RUN=1 이면 실행 목록과 예상 시간만 출력.
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
load_conf
run_experiment C
