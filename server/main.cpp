#include <iostream>
#include <string>
#include <memory>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <algorithm>
#include <curl/curl.h>

#include "Logger.h"
#include "ThreadPool.h"
#include "ZoneMapper.h"
#include "CacheManager.h"
#include "CongestionRouter.h"
#include "SpatialDensityEngine.h"
#include "EpollServer.h"
#include "PublicDataFeeder.h"
#include "SeoulCityDataClient.h"
#include "DaeguTrafficClient.h"
#include "RedisClient.h"

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

// ── 시그널 핸들러용 전역 핸들 ─────────────────────────────
static EpollServer*           g_server = nullptr;
static std::atomic<bool>      g_cleanupRunning{false};

static void handleSignal(int) {
    g_cleanupRunning = false;
    if (g_server) g_server->stop();
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    std::signal(SIGPIPE, SIG_IGN);   // send() 중 상대 종료로 인한 프로세스 종료 방지

    std::cout << "=== CrowdMap Server (epoll reactor + SpatialDensityEngine) ===\n\n";

    // 1. 환경변수 로드
    const std::string seoulApiKey = requireEnv("SEOUL_API_KEY");
    const std::string daeguApiKey = optionalEnv("DAEGU_API_KEY", "");
    const int         port        = std::stoi(optionalEnv("SERVER_PORT", "8765"));

    // ── 수명(destruction) 순서가 중요하다 ──────────────────────────────
    // 선언 순서:  zoneMapper → cacheManager → densityEngine → redisClient
    //            → router → feeder → threadPool → cleanupThread → server
    // 소멸은 역순이므로 server 가 가장 먼저, redisClient 가 densityEngine 이후에 소멸한다.

    // 2. 비즈니스 컴포넌트
    ZoneMapper           zoneMapper;
    CacheManager         cacheManager(30, 1000);
    SpatialDensityEngine densityEngine;

    // Redis 연결 및 이전 데이터 복원
    const std::string redisHost = optionalEnv("REDIS_HOST", "127.0.0.1");
    const int         redisPort = std::stoi(optionalEnv("REDIS_PORT", "6379"));
    std::unique_ptr<RedisClient> redisClient;
    try {
        redisClient = std::make_unique<RedisClient>(redisHost, redisPort);
        Log::info("Redis 연결 성공 (" + redisHost + ":" + std::to_string(redisPort) + ")");
        densityEngine.setRedis(redisClient.get());
        densityEngine.restoreFromRedis(*redisClient);
    } catch (const std::exception& e) {
        // Redis 없어도 서버는 메모리 모드로 계속 동작
        Log::err(std::string("Redis 연결 실패 - 영속성 비활성화: ") + e.what());
    }

    CongestionRouter router;
    router.addClient(std::make_shared<SeoulCityDataClient>(seoulApiKey));
    router.addClient(std::make_shared<DaeguTrafficClient>(daeguApiKey));
    Log::info("CongestionRouter ready (Seoul + Daegu registered)");

    // 3. 공공데이터 피더(아직 시작하지 않음) — 선언만 (수명 보장용)
    PublicDataFeeder feeder(densityEngine, seoulApiKey);

    // 4. ThreadPool: epoll 리액터가 넘긴 블로킹 조회 작업(외부 API 호출 등)을 처리
    const int workerCount =
            std::max(static_cast<int>(std::thread::hardware_concurrency()) * 2, 4);
    ThreadPool threadPool(workerCount);
    Log::info("ThreadPool: " + std::to_string(workerCount) + " workers"
              + " (cores=" + std::to_string(std::thread::hardware_concurrency()) + ")");

    // 5. 밀도 엔진 청소 스레드 핸들(아직 시작하지 않음)
    std::thread cleanupThread;

    // 6. epoll 서버 생성 (bind 실패 등으로 throw 가능 — 이 시점엔 아직 스레드 미시작)
    EpollServer server(static_cast<uint16_t>(port),
                       densityEngine, zoneMapper, router, cacheManager, threadPool);
    g_server = &server;

    // ── 서버 생성 성공 후에 백그라운드 스레드들을 시작 ──
    feeder.start();   // 5분 주기 서울시 API → 가상 유저 주입

    // 밀도 엔진 청소: GridZone 5분 만료 데이터/빈 그리드 제거
    g_cleanupRunning = true;
    cleanupThread = std::thread([&densityEngine]() {
        while (g_cleanupRunning) {
            for (int i = 0; i < 30 && g_cleanupRunning; ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!g_cleanupRunning) break;
            densityEngine.globalCleanup();
            Log::info("[DensityCleaner] globalCleanup 완료");
        }
    });

    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    server.run();   // stop() 전까지 블로킹

    // 7. 정상 종료
    Log::info("shutting down...");
    g_cleanupRunning = false;
    if (cleanupThread.joinable()) cleanupThread.join();
    feeder.stop();

    g_server = nullptr;
    curl_global_cleanup();
    return 0;
}
