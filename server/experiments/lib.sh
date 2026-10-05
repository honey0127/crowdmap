#!/usr/bin/env bash
# 실험 스크립트 공용 함수. exp0_calibrate.sh, expA~D.sh 가 source 한다.
#
# 서버 제어: SERVER_SSH 가 비어 있으면 같은 기계(스모크 테스트용), 있으면 ssh 로 원격 서버를
# 실행마다 새로 띄우고(상태 초기화) 끝나면 SIGTERM 으로 멈춘 뒤 기록 파일을 가져온다.
# 이어하기: 실행 디렉터리에 DONE 이 있으면 건너뛴다. 실패한 실행은 FAILED 를 남기고 다음으로 간다.

set -uo pipefail

EXP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_SRC_DIR="$(cd "$EXP_DIR/.." && pwd)"

log()  { printf '[exp %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die()  { log "오류: $*"; exit 1; }

# ── 설정 ────────────────────────────────────────────────────────────────
load_conf() {
    local conf="${EXP_CONF:-$EXP_DIR/exp.conf}"
    CONF_PATH=""
    # 명시한 설정 파일이 없으면 멈춘다: 기본값(같은 기계)으로 조용히 넘어가면 서버와 생성기가
    # 한 VM 의 CPU 를 나눠 쓰는 실험이 끝까지 돌아 버린다
    if [ -n "${EXP_CONF:-}" ] && [ ! -f "$EXP_CONF" ]; then
        die "EXP_CONF 파일 없음: $EXP_CONF"
    fi
    if [ -f "$conf" ]; then
        # shellcheck disable=SC1090
        source "$conf"
        CONF_PATH="$(cd "$(dirname "$conf")" && pwd)/$(basename "$conf")"
    else
        log "설정 파일 없음($conf) — 기본값(같은 기계) 사용. exp.conf.example 참고"
    fi

    # 서버
    : "${SERVER_SSH:=}"
    : "${SSH_OPTS:=-o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=15}"
    : "${SERVER_HOST:=127.0.0.1}"
    : "${SERVER_PORT:=8765}"
    : "${SERVER_BIN:=$SERVER_SRC_DIR/build-exp/crowdmap_server}"
    : "${SERVER_WORKDIR:=/tmp/crowdmap-exp}"
    : "${SERVER_ENV_COMMON:=FAKE_API_MS=100}"
    : "${SERVER_START_TIMEOUT_S:=20}"
    # 부하 생성기
    : "${LOADGEN_BIN:=$SERVER_SRC_DIR/build-exp/crowdmap_loadgen}"
    : "${LOADGEN_TASKSET:=}"
    : "${LG_CONNS:=20000}"
    : "${LG_THREADS:=4}"
    : "${LG_OUTSTANDING:=8}"
    : "${WARMUP:=10}"
    : "${DURATION:=75}"
    : "${TIMEOUT:=3}"
    : "${WORK_US:=10000}"
    : "${SYNC_WIDTH:=3}"
    : "${SYNC_PERIOD:=25}"
    : "${Q_THRESHOLD_MS:=50}"
    : "${SEED_BASE:=1}"
    : "${REACTOR_FRAC:=0.8}"
    : "${COOLDOWN_S:=3}"
    : "${START_OVERHEAD_S:=4}"
    : "${MAX_CONSEC_FAIL:=3}"
    : "${GENLAG_DISCARD_MS:=10}"
    # 서버 fd: 연결마다 1개 + 큐에 쌓인 작업마다 dup 한 1개. 동적 상한 최대 2^17 = 131072
    : "${SERVER_FD_QUEUE:=131072}"
    # 결과
    : "${RESULTS_DIR:=$SERVER_SRC_DIR/results}"
    : "${CALIB_FILE:=$RESULTS_DIR/calib.env}"
    : "${DRY_RUN:=0}"

    # 서버 기계에서 실행마다 rm -rf 하는 경로라 빈 값·루트를 막는다
    case "$SERVER_WORKDIR" in
        ""|"/"|"/tmp"|"$HOME"|"/home"|"/root") die "SERVER_WORKDIR 가 위험한 경로: '$SERVER_WORKDIR'";;
    esac

    mkdir -p "$RESULTS_DIR"
    RESULTS_DIR="$(cd "$RESULTS_DIR" && pwd)"

    # plan.py / analyze.py 가 읽도록 내보낸다 (실험별 변수 A_*, B_*, C_*, D_*, CAL_* 포함)
    local v
    for v in $(compgen -v | grep -E '^(SERVER_|LG_|WARMUP|DURATION|TIMEOUT|WORK_US|SYNC_|Q_THRESHOLD_MS|SEED_BASE|REACTOR_FRAC|COOLDOWN_S|START_OVERHEAD_S|RESULTS_DIR|CALIB_FILE|GENLAG_DISCARD_MS|REPS_|A_|B_|C_|D_|CAL_)'); do
        export "${v?}"
    done
}

require_calib() {
    [ -f "$CALIB_FILE" ] || die "보정 파일 없음: $CALIB_FILE — ./exp0_calibrate.sh 를 먼저 실행"
}

# ── 서버 제어 ────────────────────────────────────────────────────────────
srv_exec() {   # 서버 기계에서 셸 명령 실행
    # stdin 을 /dev/null 로: ssh 는 stdin 을 읽으므로, while read 루프 안에서 부르면
    # 남은 실행 목록을 삼켜 버린다.
    if [ -n "$SERVER_SSH" ]; then
        # shellcheck disable=SC2086
        ssh $SSH_OPTS "$SERVER_SSH" "$1" < /dev/null
    else
        bash -c "$1" < /dev/null
    fi
}

srv_fetch() {  # 서버 기계 파일 → 로컬
    if [ -n "$SERVER_SSH" ]; then
        # shellcheck disable=SC2086
        scp -q $SSH_OPTS "$SERVER_SSH:$1" "$2" < /dev/null
    else
        cp "$1" "$2"
    fi
}

port_open() {
    (exec 3<>"/dev/tcp/$SERVER_HOST/$SERVER_PORT") 2>/dev/null
}

# srv_start <run_id> <조건 env> → 표준출력에 서버 PID
srv_start() {
    local rdir="$SERVER_WORKDIR/$1"
    # { … & echo $!; } 로 서버만 백그라운드로 보낸다. 'a && b && 서버 &' 로 쓰면 && 목록 전체가
    # 서브셸로 백그라운드에 남아 표준출력 파이프를 쥐고 있어, $(…) 와 ssh 가 끝나지 않는다.
    local cmd="rm -rf '$rdir' && mkdir -p '$rdir' && cd '$rdir' && { \
env $SERVER_ENV_COMMON $2 SERVER_PORT=$SERVER_PORT CTRL_CSV='$rdir/ctrl.csv' \
nohup '$SERVER_BIN' > '$rdir/server.log' 2>&1 < /dev/null & echo \$!; }"
    srv_exec "$cmd"
}

srv_alive() { srv_exec "kill -0 $1 2>/dev/null"; }

# SIGTERM → 서버가 컨트롤러 CSV 를 비우고 끝낸다(최대 5초). 안 끝나면 SIGKILL.
srv_stop() {
    srv_exec "kill $1 2>/dev/null; for i in \$(seq 50); do kill -0 $1 2>/dev/null || exit 0; sleep 0.1; done; kill -9 $1 2>/dev/null; exit 0"
}

# wait_server_up <pid> <run_id>
# 포트가 열린 것만으로는 부족하다: 이전 서버가 아직 포트를 쥐고 있으면 새 서버는 bind 에 실패해
# 죽는데 포트는 열려 있다. 이 서버의 로그에 'listening on port' 가 찍히고 살아 있어야 '뜸'.
wait_server_up() {
    local deadline=$((SECONDS + SERVER_START_TIMEOUT_S))
    local logf="$SERVER_WORKDIR/$2/server.log"
    while [ $SECONDS -lt $deadline ]; do
        if srv_exec "grep -q 'listening on port' '$logf' 2>/dev/null && kill -0 $1 2>/dev/null"; then
            port_open && return 0
        fi
        srv_alive "$1" || return 1
        sleep 0.2
    done
    return 1
}

wait_port_free() {   # 이전 서버가 포트를 놓을 때까지 (최대 10초)
    local deadline=$((SECONDS + 10))
    while port_open; do
        [ $SECONDS -ge $deadline ] && return 1
        sleep 0.2
    done
    return 0
}

# 서버 시계 − 이 기계 시계(초). ssh 왕복의 가운데 시각과 비교한다. 3번 재서 왕복이 가장 짧은
# 표본을 쓴다(접속 지연이 클수록 가운데 가정의 오차가 커진다). 실패하면 종료 코드 1.
clock_offset() {
    if [ -z "$SERVER_SSH" ]; then echo 0; return 0; fi
    local i a r b samples=""
    for i in 1 2 3; do
        a=$(date +%s.%N)
        r=$(srv_exec 'date +%s.%N') || continue
        b=$(date +%s.%N)
        [[ "$r" =~ ^[0-9]+\.[0-9]+$ ]] || continue
        samples+="$a $r $b;"
    done
    [ -n "$samples" ] || return 1
    python3 -c "
s = [tuple(map(float, x.split())) for x in '$samples'.split(';') if x]
a, r, b = min(s, key=lambda t: t[2] - t[0])
print(f'{r - (a + b) / 2:.4f}')"
}

# 실행 서명: 서버 env, 생성기 인자, 공통 설정, 두 바이너리. 이어하기 때 이 값이 다르면
# 예전 결과(다른 보정값·창 길이·바이너리)를 쓰지 않고 다시 잰다.
plan_sig() {   # <서버 env> <생성기 인자>
    printf '%s|%s|%s|%s|%s|%s|%s' "$1" "$2" "$SERVER_ENV_COMMON" "$LG_THREADS" \
        "$LG_OUTSTANDING" "$TIMEOUT" "${BIN_SIG:-}" | sha1sum | cut -c1-16
}

# DONE 이 있고 서명이 같으면 0. 서명이 다르면 예전 결과를 _stale/ 로 옮기고 1.
run_is_done() {   # <exp> <run_id> <서명>
    local dir="$RESULTS_DIR/$1/$2" old
    [ -f "$dir/DONE" ] || return 1
    old=$(grep -E '^PLAN_SIG=' "$dir/meta.env" 2>/dev/null | cut -d= -f2)
    [ "$old" = "$3" ] && return 0
    mkdir -p "$RESULTS_DIR/$1/_stale"
    mv "$dir" "$RESULTS_DIR/$1/_stale/$2.$(date +%Y%m%d%H%M%S)"
    log "  $2: 예전 결과의 설정·바이너리가 지금 계획과 달라 _stale/ 로 옮기고 다시 잰다"
    return 1
}

CURRENT_PID=""
on_interrupt() {
    if [ -n "$CURRENT_PID" ]; then
        log "중단 — 서버(PID $CURRENT_PID) 정지"
        srv_stop "$CURRENT_PID" >/dev/null 2>&1
    fi
    exit 130
}
trap on_interrupt INT TERM HUP

# ── 사전 점검 ────────────────────────────────────────────────────────────
nofile_ok() {   # <hard limit 문자열> <필요 수>
    [ "$1" = "unlimited" ] && return 0
    [ "$1" -ge "$2" ] 2>/dev/null
}

preflight() {   # <exp>
    local exp=$1 need=$((LG_CONNS + 1000)) sneed=$((LG_CONNS + SERVER_FD_QUEUE + 1000)) h lo hi
    command -v python3 >/dev/null || die "python3 필요"
    [ -x "$LOADGEN_BIN" ] || die "부하 생성기 없음: $LOADGEN_BIN (cmake --build build-exp)"
    if [ -n "$SERVER_SSH" ]; then
        command -v ssh >/dev/null || die "ssh 필요"
        srv_exec true || die "ssh 접속 실패: $SERVER_SSH"
    fi
    srv_exec "test -x '$SERVER_BIN'" || die "서버 바이너리 없음(서버 기계): $SERVER_BIN"

    h=$(ulimit -Hn)
    nofile_ok "$h" "$need" || die "생성기 fd 한도 $h < $need (/etc/security/limits.conf 의 nofile)"
    h=$(srv_exec 'ulimit -Hn')
    nofile_ok "$h" "$sneed" || die "서버 fd 한도 $h < $sneed (연결 $LG_CONNS + 큐에 쌓인 작업마다 dup fd 최대 $SERVER_FD_QUEUE)"
    read -r lo hi < /proc/sys/net/ipv4/ip_local_port_range
    [ $((hi - lo)) -ge "$need" ] || die "생성기 임시 포트 범위 $lo-$hi 가 연결 수보다 좁음 (sysctl net.ipv4.ip_local_port_range)"

    port_open && die "$SERVER_HOST:$SERVER_PORT 에 이미 무언가 떠 있음 — 이전 서버를 정리할 것"
    clock_offset >/dev/null || die "서버 시계를 읽지 못함 (ssh 'date +%s.%N')"
    # 바이너리 지문: 실행 서명에 넣어, 바이너리가 바뀌면 예전 결과를 다시 쓰지 않게 한다
    BIN_SIG="$(srv_exec "md5sum '$SERVER_BIN'" | cut -c1-12)-$(md5sum "$LOADGEN_BIN" | cut -c1-12)"
    [ ${#BIN_SIG} -ge 25 ] || die "바이너리 지문을 만들지 못함"
    export BIN_SIG

    # 실험 빌드인지 확인: 응답에 id= 와 q= 가 붙어야 한다
    local pid line
    pid=$(srv_start "_probe" "") || die "서버 시작 실패"
    CURRENT_PID=$pid
    wait_server_up "$pid" "_probe" || {
        srv_stop "$pid"
        die "서버가 $SERVER_START_TIMEOUT_S초 안에 뜨지 않음: $(srv_exec "tail -n 3 '$SERVER_WORKDIR/_probe/server.log'" | tr '\n' ' ')"
    }
    if ! line=$(python3 - "$SERVER_HOST" "$SERVER_PORT" <<'PY'
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=5)
s.sendall(b"0,37.5636,126.9869,id=7\n")
buf = b""
while b"\n" not in buf:
    d = s.recv(4096)
    if not d:
        break
    buf += d
line = buf.decode(errors="replace").strip()
print(line)
sys.exit(0 if "|id=7" in line and ("|q=" in line or "|x=" in line) else 1)
PY
    ); then
        srv_stop "$pid"; CURRENT_PID=""
        die "실험 빌드가 아님(응답: ${line:-없음}) — cmake -DCROWDMAP_EXPERIMENT=ON 으로 빌드"
    fi
    srv_stop "$pid"; CURRENT_PID=""

    mkdir -p "$RESULTS_DIR/$exp"
    {
        echo "# 생성기"
        echo "host: $(hostname)  git: $(git -C "$SERVER_SRC_DIR" rev-parse --short HEAD 2>/dev/null)$(git -C "$SERVER_SRC_DIR" diff --quiet 2>/dev/null || echo '+변경')"
        echo "nproc: $(nproc)"; lscpu 2>/dev/null | grep -E 'Model name|Thread|Core|Socket'
        echo "loadgen md5: $(md5sum "$LOADGEN_BIN" | cut -d' ' -f1)"
        echo "# 서버 ($SERVER_SSH)"
        srv_exec "hostname; echo nproc: \$(nproc); lscpu 2>/dev/null | grep -E 'Model name|Thread|Core|Socket'; md5sum '$SERVER_BIN'"
        echo "clock_offset_s: $(clock_offset)"
        echo "conf: ${CONF_PATH:-기본값}"
    } > "$RESULTS_DIR/$exp/environment.txt" 2>&1
    log "사전 점검 통과 (응답 예: $line)"
}

# ── 실행 1회 ─────────────────────────────────────────────────────────────
# run_one <exp> <run_id> <서버 env> <생성기 인자> <예상 초> <meta K=V ...>
# 반환: 0 완료(또는 이미 완료), 1 실패
run_one() {
    local exp=$1 run_id=$2 senv=$3 lgargs=$4 expect=$5 metakv=$6
    local dir="$RESULTS_DIR/$exp/$run_id" sig
    sig=$(plan_sig "$senv" "$lgargs")
    if run_is_done "$exp" "$run_id" "$sig"; then
        return 0
    fi
    rm -rf "$dir"; mkdir -p "$dir"

    local offset pid rc
    if ! offset=$(clock_offset); then
        echo "서버 시계 측정 실패(ssh)" > "$dir/FAILED"
        log "  실패: 서버 시계를 읽지 못함"
        return 1
    fi
    {
        echo "PLAN_SIG=$sig"
        echo "BIN_SIG=${BIN_SIG:-}"
        echo "EXP=$exp"
        echo "RUN_ID=$run_id"
        echo "SERVER_ENV=\"$senv\""
        echo "SERVER_ENV_COMMON=\"$SERVER_ENV_COMMON\""
        echo "LG_ARGS=\"$lgargs --threads $LG_THREADS --outstanding $LG_OUTSTANDING --timeout $TIMEOUT\""
        echo "CLOCK_OFFSET_S=$offset"
        echo "GIT_REV=$(git -C "$SERVER_SRC_DIR" rev-parse --short HEAD 2>/dev/null)"
        echo "STARTED_AT=$(date -Iseconds)"
        local kv
        for kv in $metakv; do echo "$kv"; done
    } > "$dir/meta.env"

    if ! wait_port_free; then
        echo "포트 $SERVER_PORT 가 10초 넘게 사용 중" > "$dir/FAILED"
        log "  실패: 포트 $SERVER_PORT 가 사용 중 — 남은 서버 프로세스를 정리할 것"
        return 1
    fi
    pid=$(srv_start "$run_id" "$senv") || { echo "서버 시작 명령 실패" > "$dir/FAILED"; return 1; }
    CURRENT_PID=$pid
    if ! wait_server_up "$pid" "$run_id"; then
        srv_stop "$pid" >/dev/null 2>&1; CURRENT_PID=""
        srv_fetch "$SERVER_WORKDIR/$run_id/server.log" "$dir/server.log" 2>/dev/null
        echo "서버가 뜨지 않음" > "$dir/FAILED"
        log "  실패: 서버가 뜨지 않음 — $(tail -n 3 "$dir/server.log" 2>/dev/null | tr '\n' ' ')"
        return 1
    fi

    # 생성기가 멈추는 경우를 대비한 상한: 예상 시간 + 2분
    # shellcheck disable=SC2086
    # --foreground: 생성기를 이 셸의 전경 프로세스 그룹에 둬 Ctrl-C 가 바로 닿게 한다
    # (없으면 timeout 이 따로 프로세스 그룹을 만들어 Ctrl-C 가 이번 실행이 끝날 때까지 미뤄진다)
    timeout --foreground $((expect + 120)) $LOADGEN_TASKSET "$LOADGEN_BIN" < /dev/null \
        --host "$SERVER_HOST" --port "$SERVER_PORT" \
        --threads "$LG_THREADS" --outstanding "$LG_OUTSTANDING" --timeout "$TIMEOUT" \
        $lgargs --label "$run_id" --out "$dir/lg" > "$dir/loadgen.log" 2>&1
    rc=$?
    echo "LG_RC=$rc" >> "$dir/meta.env"

    local alive=1
    srv_alive "$pid" || alive=0
    srv_stop "$pid" >/dev/null 2>&1; CURRENT_PID=""
    srv_fetch "$SERVER_WORKDIR/$run_id/ctrl.csv" "$dir/ctrl.csv" 2>/dev/null
    srv_fetch "$SERVER_WORKDIR/$run_id/server.log" "$dir/server.log" 2>/dev/null
    srv_exec "rm -rf '$SERVER_WORKDIR/$run_id'" >/dev/null 2>&1

    if [ "$alive" -eq 0 ]; then
        echo "실행 중 서버가 죽음" > "$dir/FAILED"
        log "  실패: 실행 중 서버가 죽음 — $(tail -n 3 "$dir/server.log" 2>/dev/null | tr '\n' ' ')"
        return 1
    fi
    if [ "$rc" -ne 0 ] || [ ! -s "$dir/lg_summary.csv" ]; then
        echo "생성기 종료 코드 $rc" > "$dir/FAILED"
        log "  실패: 생성기 종료 코드 $rc — $(tail -n 2 "$dir/loadgen.log" | tr '\n' ' ')"
        return 1
    fi
    if [ ! -s "$dir/ctrl.csv" ]; then
        echo "컨트롤러 CSV 없음" > "$dir/FAILED"
        log "  실패: 컨트롤러 CSV 를 가져오지 못함"
        return 1
    fi
    grep -E "^gen_lag|경고" "$dir/loadgen.log" | sed 's/^/    /' >&2
    touch "$dir/DONE"
    sleep "$COOLDOWN_S"
    return 0
}

# ── 계획 실행 ────────────────────────────────────────────────────────────
# run_plan <exp> <plan.txt>
run_plan() {
    local exp=$1 plan=$2
    local total done_n=0 fail_n=0 consec=0 i=0 remain_s=0
    local -a ids envs args exps metas
    while IFS='|' read -r id senv lgargs expect meta; do
        ids+=("$id"); envs+=("$senv"); args+=("$lgargs"); exps+=("$expect"); metas+=("$meta")
        [ -f "$RESULTS_DIR/$exp/$id/DONE" ] || remain_s=$((remain_s + expect))
    done < "$plan"
    # 남은 시간은 DONE 만 보고 어림한다(서명이 달라 다시 잴 실행은 돌면서 더해진다)
    total=${#ids[@]}
    log "실험 $exp: $total회 중 남은 실행 예상 $((remain_s / 60))분"
    if [ "$DRY_RUN" = "1" ]; then
        for ((i = 0; i < total; i++)); do
            printf '  %-28s | %s | %s\n' "${ids[$i]}" "${envs[$i]}" "${args[$i]}" >&2
        done
        return 0
    fi

    for ((i = 0; i < total; i++)); do
        local id=${ids[$i]}
        if run_is_done "$exp" "$id" "$(plan_sig "${envs[$i]}" "${args[$i]}")"; then
            done_n=$((done_n + 1)); continue
        fi
        log "[$exp $((i + 1))/$total] $id (남은 약 $((remain_s / 60))분)"
        if run_one "$exp" "$id" "${envs[$i]}" "${args[$i]}" "${exps[$i]}" "${metas[$i]}"; then
            done_n=$((done_n + 1)); consec=0
        else
            fail_n=$((fail_n + 1)); consec=$((consec + 1))
            if [ "$consec" -ge "$MAX_CONSEC_FAIL" ]; then
                die "연속 $consec회 실패 — 환경 문제로 보고 중단 ($RESULTS_DIR/$exp/*/FAILED 확인)"
            fi
        fi
        remain_s=$((remain_s - ${exps[$i]}))
    done
    log "실험 $exp 끝: 완료 $done_n / 실패 $fail_n / 전체 $total"
    [ "$fail_n" -eq 0 ]
}

# make_plan <plan.py 대상> <출력 파일> <로그 파일>  (제외·경고는 로그에 남겨 보고서에 싣는다)
make_plan() {
    if ! python3 "$EXP_DIR/plan.py" runs "$1" > "$2" 2> "$3"; then
        cat "$3" >&2
        die "계획 생성 실패 ($1)"
    fi
    cat "$3" >&2
}

# run_experiment <A|B|C|D> : 계획 → 사전 점검 → 실행 → 보고서
run_experiment() {
    local exp=$1 out="$RESULTS_DIR/$1" rc=0
    require_calib
    mkdir -p "$out"
    make_plan "$exp" "$out/plan.txt" "$out/plan.log"
    if [ "$DRY_RUN" = "1" ]; then
        run_plan "$exp" "$out/plan.txt"
        return 0
    fi
    preflight "$exp"
    run_plan "$exp" "$out/plan.txt" || rc=1
    python3 "$EXP_DIR/analyze.py" report "$exp" || rc=1
    [ -f "$out/summary.md" ] && cat "$out/summary.md" >&2
    return $rc
}
