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

static EpollServer*      g_server = nullptr;
static std::atomic<bool> g_cleanupRunning{false};

static void handleSignal(int) {
    g_cleanupRunning = false;
    if (g_server) g_server->stop();
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    std::signal(SIGPIPE, SIG_IGN);

    std::cout << "=== CrowdMap Server (epoll reactor + SpatialDensityEngine) ===\n\n";

    // 1. 환경변수 로드
    const std::string seoulApiKey = requireEnv("SEOUL_API_KEY");
    const std::string daeguApiKey = optionalEnv("DAEGU_API_KEY", "");
    const int         port        = std::stoi(optionalEnv("SERVER_PORT", "8765"));

    // 2. 비즈니스 컴포넌트
    ZoneMapper           zoneMapper;
    CacheManager         cacheManager(60, 2000);  // [성능] TTL 30→60초, 최대 1000→2000개
    SpatialDensityEngine densityEngine;

    // Redis 연결 및 이전 데이터 복원
    const std::string redisHost = optionalEnv("REDIS_HOST", "127.0.0.1");
    const int         redisPort = std::stoi(optionalEnv("REDIS_PORT", "6379"));
    std::unique_ptr<RedisClient> redisClient;
    try {
        redisClient = std::make_unique<RedisClient>(redisHost, redisPort);
        Log::info("Redis 연결 성공 (" + redisHost + ":" + std::to_string(redisPort) + ")");
        densityEngine.setRedis(redisClient.get());
        const size_t restored = densityEngine.restoreFromRedis(*redisClient);
        Log::info("Redis 복원 완료: 이벤트 " + std::to_string(restored) + "건");
    } catch (const std::exception& e) {
        Log::err(std::string("Redis 연결 실패 - 영속성 비활성화: ") + e.what());
    }

    CongestionRouter router;
    router.addClient(std::make_shared<SeoulCityDataClient>(seoulApiKey));
    router.addClient(std::make_shared<DaeguTrafficClient>(daeguApiKey));
    Log::info("CongestionRouter ready (Seoul + Daegu registered)");

    // 3. 공공데이터 피더
    PublicDataFeeder feeder(densityEngine, seoulApiKey);

    // 4. ThreadPool: I/O 바운드 작업 많으므로 코어 수 × 4 (기존 × 2 대비 대규모 성능 향상)
    const int workerCount =
            std::max(static_cast<int>(std::thread::hardware_concurrency()) * 4, 8);
    ThreadPool threadPool(workerCount);
    Log::info("ThreadPool: " + std::to_string(workerCount) + " workers"
              + " (cores=" + std::to_string(std::thread::hardware_concurrency()) + ")");

    // 5. 밀도 엔진 청소 스레드
    std::thread cleanupThread;

    // 6. epoll 서버 생성
    EpollServer server(static_cast<uint16_t>(port),
                       densityEngine, zoneMapper, router, cacheManager, threadPool);
    g_server = &server;

    feeder.start();

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

    server.run();

    // 7. 정상 종료
    Log::info("shutting down...");
    g_cleanupRunning = false;
    if (cleanupThread.joinable()) cleanupThread.join();
    feeder.stop();

    // densityEngine(먼저 선언)보다 redisClient(나중 선언)가 먼저 소멸하므로,
    // 소멸자에 맡기지 말고 여기서 writer 스레드를 명시적으로 중지한다.
    densityEngine.stopRedisPersistence();

    g_server = nullptr;
    curl_global_cleanup();
    return 0;
}