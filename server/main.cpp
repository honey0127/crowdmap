#include <iostream>
#include <thread>
#include <string>
#include <sstream>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <mutex>
#include <algorithm> // std::max를 위해 추가
#include <curl/curl.h>
#include "Logger.h"
#include "CacheManager.h"
#include "CongestionRouter.h"
#include "ThreadPool.h"
#include "ZoneMapper.h"
#include "SpatialDensityEngine.h"
#include "SeoulCityDataClient.h"
#include "EpollServer.h"
#include "PublicDataFeeder.h"

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

// ═══════════════════════════════════════════════════════════════
// 환경변수 헬퍼
// ═══════════════════════════════════════════════════════════════
static std::string requireEnv(const char* key) {
    const char* val = std::getenv(key);
    if (!val) {
        Log::err(std::string("필수 환경변수 없음: ") + key);
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
// ═══════════════════════════════════════════════════════════════
static std::unique_ptr<ThreadPool>        threadPool;
static std::unique_ptr<CongestionRouter>  router;
static ZoneMapper                         zoneMapper;
static SpatialDensityEngine               densityEngine;
static CacheManager                       cacheManager(30, 1000);

// ═══════════════════════════════════════════════════════════════
// 서버 시작 및 루프
// ═══════════════════════════════════════════════════════════════
static void startServer(int port) {
#ifdef _WIN32
    Log::err("EpollServer is only supported on Linux.");
#else
    EpollServer server(static_cast<uint16_t>(port),
                       densityEngine,
                       zoneMapper,
                       *router,
                       cacheManager);
    server.run();
#endif
}

// ═══════════════════════════════════════════════════════════════
// 엔트리포인트
// ═══════════════════════════════════════════════════════════════
int main() {
    curl_global_init(CURL_GLOBAL_ALL);

    std::cout << "=== CrowdMap Spatial-Concurrency Server ===\n\n";

    const std::string seoulApiKey = requireEnv("SEOUL_API_KEY");
    const int port = std::stoi(optionalEnv("SERVER_PORT", "5001"));

    const int workerCount = std::max(
            static_cast<int>(std::thread::hardware_concurrency()) * 2, 4);
    threadPool = std::make_unique<ThreadPool>(workerCount);
    Log::info("ThreadPool: " + std::to_string(workerCount) + " workers");

    router = std::make_unique<CongestionRouter>();
    router->addClient(std::make_shared<SeoulCityDataClient>(seoulApiKey));

    // 1. 공공 데이터 피더 (PublicDataFeeder) 객체 생성 및 백그라운드 스레드 시작
    // 5분마다 서울 API 데이터를 가져와 SpatialDensityEngine에 주입합니다.
    PublicDataFeeder dataFeeder(densityEngine, seoulApiKey);
    dataFeeder.start();

    // 2. 백그라운드 클린업 스레드 시작 (기존 코드의 컴파일 에러 문법 수정 완료)
    std::thread cleanupThread([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            densityEngine.globalCleanup();
            Log::info("Global spatial cleanup performed");
        }
    });
    cleanupThread.detach();

    // 3. Linux Epoll 기반 서버 시작 (이 함수 내부의 server.run()에서 블로킹되어 무한 루프가 돕니다)
    Log::info("Starting EpollServer on port " + std::to_string(port) + "...");
    startServer(port);

    // 4. 서버가 정상 종료 신호를 받거나 루프를 빠져나왔을 때 피더 안전하게 정지
    dataFeeder.stop();
    curl_global_cleanup();

    return 0;
}