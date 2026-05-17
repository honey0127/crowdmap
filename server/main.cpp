#include <iostream>
#include <thread>
#include <string>
#include <sstream>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <mutex>
#include <curl/curl.h>
#include "Logger.h"
#include "CacheManager.h"
#include "SlidingWindow.h"
#include "DeadSessionSweeper.h"
#include "CongestionRouter.h"

#ifdef _WIN32
#include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR   -1
#define closesocket    close
typedef int SOCKET;
#endif

#include "ThreadPool.h"
#include "ZoneMapper.h"
#include "UserCountManager.h"
#include "CongestionCalculator.h"
#include "SeoulCityDataClient.h"

// ═══════════════════════════════════════════════════════════════
// 환경변수 헬퍼
//   - 비밀값(API 키, 프로젝트 ID)을 소스코드에 하드코딩하지 않음
//   - requireEnv: 없으면 즉시 종료 (필수값)
//   - optionalEnv: 없으면 defaultVal 사용 (선택값)
// ═══════════════════════════════════════════════════════════════
static std::string requireEnv(const char* key) {
    const char* val = std::getenv(key);
    if (!val) {
        Log::err(std::string("필수 환경변수 없음: ") + key
                 + " — start.sh 또는 .env 확인");
        std::exit(1);
    }
    return val;
}

static std::string optionalEnv(const char* key, const std::string& defaultVal) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : defaultVal;
}

// ═══════════════════════════════════════════════════════════════
// 글로벌 싱글턴
//   스레드마다 생성하면 메모리/초기화 비용이 폭증하므로 전역 단일 인스턴스
// ═══════════════════════════════════════════════════════════════
static std::unique_ptr<ThreadPool>        threadPool;
static std::unique_ptr<CongestionRouter>  router;

static ZoneMapper       zoneMapper;
static UserCountManager userCountManager;
static CacheManager     cacheManager(30, 1000); // TTL=30s, LRU maxSize=1000
static SlidingWindow    slidingWindow(300);  // 윈도우 = 5분

// ═══════════════════════════════════════════════════════════════
// Cache Stampede 방지 — zone 별 fetch 락
//
//   문제: 인기 zone 의 캐시가 만료되는 순간 수백 개 스레드가
//         동시에 캐시 MISS → 외부 API 수백 회 중복 호출
//
//   해결: zone 별 mutex 로 fetch 를 1회로 직렬화
//         (1) 외부에서 캐시 MISS 확인
//         (2) zone mutex 획득
//         (3) 락 안에서 캐시 재확인 (double-check)
//         (4) 여전히 MISS 일 때만 API 호출
// ═══════════════════════════════════════════════════════════════
static std::mutex                                            zoneMutexMapLock;
static std::unordered_map<int, std::shared_ptr<std::mutex>> zoneMutexMap;

static std::shared_ptr<std::mutex> getZoneMutex(int zoneId) {
    std::lock_guard<std::mutex> lock(zoneMutexMapLock);
    auto& ptr = zoneMutexMap[zoneId];
    if (!ptr) ptr = std::make_shared<std::mutex>();
    return ptr;
}

// ═══════════════════════════════════════════════════════════════
// TCP 한 줄 읽기 — 버퍼 분할 수신(fragmentation) 대응
//
//   문제: TCP 는 스트림 프로토콜 → "1001,37.5,127.0\n" 이
//         "1001,37" / ".5,127.0\n" 처럼 쪼개져 올 수 있음
//
//   해결: \n 이 도달할 때까지 1바이트씩 누적
//         (메시지가 짧으므로 syscall 비용이 문제되지 않음)
//
//   반환: 수신한 한 줄 (개행 제외), 연결 끊김이면 빈 문자열
// ═══════════════════════════════════════════════════════════════
static std::string recvLine(SOCKET sock) {
    std::string line;
    char c;
    while (true) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return "";  // 연결 끊김 또는 에러
        if (c == '\n') break;
        if (c != '\r') line += c;  // \r\n 도 처리
    }
    return line;
}

// ═══════════════════════════════════════════════════════════════
// 클라이언트 핸들러
//   ThreadPool 워커 스레드에서 실행
//   persistent connection: 소켓 하나로 반복 수신/송신
// ═══════════════════════════════════════════════════════════════
static void handleClient(SOCKET clientSocket, int clientId) {
    const std::string cid = "[Client " + std::to_string(clientId) + "] ";

    try {
        while (true) {
            // ── 수신 ──────────────────────────────────────────
            std::string line = recvLine(clientSocket);
            if (line.empty()) {
                Log::info(cid + "disconnected");
                break;
            }

            // ── 파싱: "userId,latitude,longitude" ─────────────
            std::istringstream iss(line);
            int    userId;
            double latitude, longitude;
            char   comma;

            if (!(iss >> userId >> comma >> latitude >> comma >> longitude)) {
                Log::warn(cid + "invalid format → \"" + line + "\"");
                continue;
            }

            Log::info(cid + "userId=" + std::to_string(userId)
                      + " lat=" + std::to_string(latitude)
                      + " lng=" + std::to_string(longitude));

            // ── zone 변환 + 유효성 검사 ───────────────────────
            // ZoneMapper 가 -1 을 반환하면 지원 범위 밖 (한반도 외 극외곽 좌표)
            // Seoul API 적용 여부는 SeoulCityDataClient.covers() 에서 판단
            int zoneId = zoneMapper.coordinateToZoneId(latitude, longitude);
            if (zoneId == -1) {
                Log::warn(cid + "out-of-ZoneMapper-range coord → RELAXED");
                const std::string resp = "RELAXED|0.0\n";
                send(clientSocket, resp.c_str(), static_cast<int>(resp.length()), 0);
                continue;
            }

            // ── 사용자 수 갱신 (userId=0 은 조회 전용) ────────
            if (userId != 0) {
                slidingWindow.addEvent(userId, zoneId);
                userCountManager.updateUserLocation(userId, zoneId);
            }
            int zoneCount = userCountManager.getZoneCount(zoneId);

            // ── 혼잡도 조회 ───────────────────────────────────
            // 우선순위: 캐시 HIT → (zone 락) → 외부 API → 내부 계산 fallback
            CongestionResult result;

            if (cacheManager.get(zoneId, result)) {
                // 캐시 HIT: API 호출 없이 즉시 응답
                Log::info(cid + "cache HIT zone=" + std::to_string(zoneId));

            } else {
                // 캐시 MISS: zone 별 mutex 로 fetch 를 1회로 직렬화
                auto zoneMtx = getZoneMutex(zoneId);
                std::unique_lock<std::mutex> fetchLock(*zoneMtx);

                // double-check: 락 대기 중 다른 스레드가 채웠을 수 있음
                if (!cacheManager.get(zoneId, result)) {
                    Log::info(cid + "cache MISS, fetching zone="
                              + std::to_string(zoneId));
                    // CongestionRouter: SeoulAPI 시도 → 실패 시 내부 계산
                    // 캐시 저장도 router 내부에서 수행
                    result = router->resolve(
                            latitude, longitude, zoneCount, cacheManager, zoneId);
                }
            }

            // ── 응답 전송 ─────────────────────────────────────
            std::string response = std::string(result.levelString()) + "|"
                                   + std::to_string(result.ratio) + "\n";

            if (send(clientSocket,
                     response.c_str(),
                     static_cast<int>(response.length()), 0) == SOCKET_ERROR) {
                Log::err(cid + "send failed");
                break;
            }
            Log::info(cid + "→ " + response);
        }

    } catch (const std::exception& e) {
        Log::err(cid + "exception: " + e.what());
    }

    closesocket(clientSocket);
}

// ═══════════════════════════════════════════════════════════════
// TCP 서버 루프
//   메인 스레드에서 accept 만 담당, 실제 처리는 ThreadPool 에 위임
// ═══════════════════════════════════════════════════════════════
static void runServer(int port) {
    SOCKET serverSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        Log::err("socket creation failed");
        return;
    }

    // 재시작 시 "Address already in use" 방지
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&opt), sizeof(opt));

    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port        = htons(static_cast<uint16_t>(port));

    if (bind(serverSocket,
             reinterpret_cast<struct sockaddr*>(&serverAddr),
             sizeof(serverAddr)) == SOCKET_ERROR) {
        Log::err("bind failed on port " + std::to_string(port));
        closesocket(serverSocket);
        return;
    }

    // backlog 5→10: 동시 연결 폭증 시 accept 대기열 여유 확보
    if (listen(serverSocket, 10) == SOCKET_ERROR) {
        Log::err("listen failed");
        closesocket(serverSocket);
        return;
    }

    Log::info("server listening on port " + std::to_string(port));

    int clientIdCounter = 1;
    while (true) {
        SOCKET clientSocket = accept(
                serverSocket,
                reinterpret_cast<struct sockaddr*>(&clientAddr),
                &clientAddrLen);

        if (clientSocket == INVALID_SOCKET) {
            Log::err("accept failed");
            continue;
        }

        int cid = clientIdCounter++;
        Log::info("client " + std::to_string(cid)
                  + " connected from " + inet_ntoa(clientAddr.sin_addr));

        threadPool->enqueue([clientSocket, cid]() {
            handleClient(clientSocket, cid);
        });
    }

    closesocket(serverSocket);
}

// ═══════════════════════════════════════════════════════════════
// 엔트리포인트
// ═══════════════════════════════════════════════════════════════
int main() {
    // libcurl 전역 초기화 — 멀티스레드 환경에서 첫 번째로 반드시 호출해야 함
    // 미호출 시 OpenSSL 내부 초기화가 스레드 안전하지 않아 SIGKILL 유발
    curl_global_init(CURL_GLOBAL_ALL);

    std::cout << "=== CrowdMap Server ===\n\n";

    // 1. 환경변수에서 비밀값 로드
    //    API 키, 프로젝트 ID 를 소스코드에 절대 하드코딩하지 않음
    const std::string seoulApiKey = requireEnv("SEOUL_API_KEY");
    const int port = std::stoi(optionalEnv("SERVER_PORT", "8765"));

    // 2. ThreadPool 동적 크기
    //    I/O 바운드 작업(Seoul API HTTP) 이 많으므로 코어 수 × 2 가 효율적
    //    hardware_concurrency() 가 0 을 반환하는 환경 대비 최소 4 보장
    const int workerCount = std::max(
            static_cast<int>(std::thread::hardware_concurrency()) * 2, 4);
    threadPool = std::make_unique<ThreadPool>(workerCount);
    Log::info("ThreadPool: " + std::to_string(workerCount) + " workers"
              + " (cores=" + std::to_string(std::thread::hardware_concurrency()) + ")");

    // 3. CongestionRouter 구성
    //    우선순위: SeoulCityDataClient → (fallback) 내부 계산
    router = std::make_unique<CongestionRouter>();
    router->addClient(std::make_shared<SeoulCityDataClient>(seoulApiKey));
    Log::info("CongestionRouter ready (SeoulCityDataClient registered)");

    // 4. 고스트 유저 정리 백그라운드 스레드 시작
    DeadSessionSweeper deadSweeper(userCountManager, slidingWindow, 300);
    deadSweeper.start();

    // 5. 서버 시작 (메인 스레드는 accept 루프만 담당)
    runServer(port);

    return 0;
}