#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  CrowdMap Server 시작 스크립트
# ═══════════════════════════════════════════════════════════════
set -euo pipefail  # 에러 발생 시 즉시 종료, 미정의 변수 참조 금지

# ── 경로 설정 ────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
BINARY="$BUILD_DIR/crowdmap_server"
LOG_DIR="$REPO_ROOT/logs"
ENV_FILE="$REPO_ROOT/.env"  # 비밀값 보관 파일 (.gitignore 에 반드시 추가)

# ── .env 파일 로드 ───────────────────────────────────────────────
# .env 형식:
#   SEOUL_API_KEY=실제키값
#   FIREBASE_PROJECT_ID=crowdmap-50936
#   SERVER_PORT=5001
if [ -f "$ENV_FILE" ]; then
    # 주석(#) 과 빈 줄을 제외하고 export
    set -a
    # shellcheck disable=SC1090
    source <(grep -E '^[A-Z_]+=.+' "$ENV_FILE")
    set +a
    echo "[start.sh] .env 로드 완료: $ENV_FILE"
else
    echo "[start.sh] WARNING: .env 파일 없음 ($ENV_FILE)"
    echo "           환경변수가 이미 설정되어 있지 않으면 서버가 종료됩니다"
fi

# ── 필수 환경변수 사전 확인 ─────────────────────────────────────
# 서버 프로세스 안에서 확인하기 전에 스크립트 단에서 먼저 검사
MISSING=0
for VAR in SEOUL_API_KEY; do
    if [ -z "${!VAR:-}" ]; then
        echo "[start.sh] ERROR: 필수 환경변수 없음 → $VAR"
        MISSING=1
    fi
done
if [ "$MISSING" -eq 1 ]; then
    echo ""
    echo "  .env 파일 예시:"
    echo "    SEOUL_API_KEY=여기에_서울_API_키"
    echo "    SERVER_PORT=5001"
    exit 1
fi

# 기본값 설정
export SERVER_PORT="${SERVER_PORT:-8765}"

# ── 바이너리 사전 확인 ───────────────────────────────────────────
if [ ! -f "$BINARY" ]; then
    echo "[start.sh] ERROR: 바이너리 없음 → $BINARY"
    echo ""
    echo "  빌드 방법:"
    echo "    cd $REPO_ROOT"
    echo "    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
    echo "    cmake --build build -j\$(nproc)"
    exit 1
fi

if [ ! -x "$BINARY" ]; then
    echo "[start.sh] ERROR: 실행 권한 없음 → $BINARY"
    echo "  chmod +x $BINARY"
    exit 1
fi

# ── 로그 디렉터리 생성 ───────────────────────────────────────────
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/server_$(date +%Y%m%d_%H%M%S).log"

# ── 시작 정보 출력 ───────────────────────────────────────────────
CORE_COUNT="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo '?')"
WORKER_COUNT=$(( CORE_COUNT * 2 ))

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║          CrowdMap Server 시작            ║"
echo "╠══════════════════════════════════════════╣"
echo "║  바이너리  : $BINARY"
echo "║  포트      : $SERVER_PORT"
echo "║  CPU 코어  : $CORE_COUNT  →  워커 스레드: $WORKER_COUNT"
echo "║  로그 파일 : $LOG_FILE"
echo "╚══════════════════════════════════════════╝"
echo ""

# ── 서버 실행 ────────────────────────────────────────────────────
# exec: 현재 쉘을 서버 프로세스로 교체 (불필요한 부모 프로세스 제거)
# tee:  터미널 실시간 출력 + 파일 동시 저장
exec "$BINARY" 2>&1 | tee "$LOG_FILE"