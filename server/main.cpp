#include <iostream>
#include <string>
#include <memory>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <algorithm>
#include <sys/resource.h>
#include <curl/curl.h>

#include "Logger.h"
#include "ClassPool.h"
#include "CpuTopology.h"
#include "PoolController.h"
#include "QueryService.h"
#include "ZoneMapper.h"
#include "CacheManager.h"
#include "CongestionRouter.h"
#include "SpatialDensityEngine.h"
#include "EpollServer.h"
#include "PublicDataFeeder.h"
#include "SeoulCityDataClient.h"
#include "DaeguTrafficClient.h"
#include "FakeCongestionClient.h"
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

static int envInt(const char* key, int def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

static double envDouble(const char* key, double def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
    try { return std::stod(v); } catch (...) { return def; }
}

/**
 * 조회 큐·워커 설정 (환경변수). 기본값 = v5 설계.
 *
 *   POOL_PROFILE  legacy        → v4 재현: 공용 큐 1개, 상한 10,000 고정, 워커 max(코어×4, 8) 고정,
 *                                 외부 API 동기 호출. 아래 개별 변수로 덮어쓸 수 있다.
 *   POOL_LAYOUT   split|shared  요청 종류별 큐 / 공용 큐 1개              (split)
 *   CAP_MODE      dynamic|fixed K = c·μ̂·1초 / K = QUEUE_CAP               (dynamic)
 *   QUEUE_CAP     fixed 모드 상한                                          (10000)
 *   SCALING       adaptive|fixed 컨트롤러가 c 조절 / c = WORKERS 고정      (adaptive)
 *   WORKERS       fixed 모드 스레드 수                    (split: 워커 코어 수, shared: max(코어×4, 8))
 *   WORKER_MIN / WORKER_MAX   adaptive 모드 c 범위         (1 / 물리 코어 − 1)
 *   PIN / REACTOR_CPU / WORKER_CPUS   코어 고정             (auto / 첫 물리 코어 / 나머지)
 *   COMPLEX_NICE  COMPLEX 스레드 nice                        (5)
 *   ROUTER_MODE   async|sync    외부 API 를 REFRESH 큐로 / 조회 경로에서 직접 (async)
 *   FAKE_API_MS   설정 시 실제 외부 API 대신 이 지연의 가짜 API (실험 D)
 *   MU0_SIMPLE / MU0_COMPLEX  처리 표본이 없을 때의 μ(스레드당 /s) — 실험 0 보정값 (33333 / 100)
 *   CTRL_CSV      설정 시 10ms 컨트롤러 기록 경로
 */
struct PoolSettings {
    bool        split       = true;
    bool        dynamicCap  = true;
    size_t      fixedCap    = 10'000;
    bool        adaptive    = true;
    int         workers     = 0;
    int         cMin        = 1;
    int         cMax        = 1;
    int         complexNice = 5;
    bool        asyncRouter = true;
    int         fakeApiMs   = 0;
    double      mu0Simple   = 33'333.0;   // 30µs
    double      mu0Complex  = 100.0;      // 10ms
    std::string csvPath;

    static PoolSettings fromEnv(int workerCores) {
        PoolSettings p;
        const int hw = static_cast<int>(std::thread::hardware_concurrency());
        const int legacyWorkers = std::max(hw * 4, 8);   // v4 main.cpp 와 같은 식

        const bool legacy = optionalEnv("POOL_PROFILE", "") == "legacy";
        if (legacy) {
            p.split       = false;
            p.dynamicCap  = false;
            p.adaptive    = false;
            p.asyncRouter = false;
        }
        p.split       = optionalEnv("POOL_LAYOUT", p.split ? "split" : "shared") == "split";
        p.dynamicCap  = optionalEnv("CAP_MODE", p.dynamicCap ? "dynamic" : "fixed") == "dynamic";
        p.fixedCap    = static_cast<size_t>(std::max(1, envInt("QUEUE_CAP", 10'000)));
        p.adaptive    = optionalEnv("SCALING", p.adaptive ? "adaptive" : "fixed") == "adaptive";
        p.workers     = envInt("WORKERS", p.split ? workerCores : legacyWorkers);
        p.workers     = std::max(1, p.workers);
        p.cMin        = std::max(1, envInt("WORKER_MIN", 1));
        p.cMax        = std::max(p.cMin, envInt("WORKER_MAX", workerCores));
        p.complexNice = envInt("COMPLEX_NICE", 5);
        p.asyncRouter = optionalEnv("ROUTER_MODE", p.asyncRouter ? "async" : "sync") == "async";
        p.fakeApiMs   = std::max(0, envInt("FAKE_API_MS", 0));
        p.mu0Simple   = envDouble("MU0_SIMPLE", p.mu0Simple);
        p.mu0Complex  = envDouble("MU0_COMPLEX", p.mu0Complex);
        p.csvPath     = optionalEnv("CTRL_CSV", "");
        return p;
    }

    int threads() const { return adaptive ? cMax : workers; }
    int initialActive() const { return adaptive ? cMin : workers; }

    std::string describe() const {
        return std::string("layout=") + (split ? "split" : "shared")
               + " cap=" + (dynamicCap ? "dynamic" : "fixed:" + std::to_string(fixedCap))
               + " scaling=" + (adaptive ? "adaptive:" + std::to_string(cMin) + "-"
                                                + std::to_string(cMax)
                                         : "fixed:" + std::to_string(workers))
               + " router=" + (asyncRouter ? "async" : "sync")
               + (fakeApiMs ? " fake_api=" + std::to_string(fakeApiMs) + "ms" : "")
               + " mu0=" + std::to_string(static_cast<int>(mu0Simple)) + "/"
               + std::to_string(static_cast<int>(mu0Complex))
               + " complex_nice=" + std::to_string(complexNice);
    }
};

// 연결 2만 개 이상을 받으려면 fd 한도가 연결 수보다 커야 한다. soft 를 hard 까지 올린다.
static void raiseFdLimit() {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
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
#ifdef CROWDMAP_EXPERIMENT
    Log::warn("실험 빌드: 요청의 w=(합성 작업량)·id= 필드를 받고 응답에 q=/s= 를 붙인다. 운영에 쓰지 말 것");
#endif

    // 0. CPU 배치: 리액터 코어를 먼저 이 스레드에 고정해 두면, 이후 만드는 배경 스레드
    //    (Redis writer, 피더, 청소, REFRESH I/O, 컨트롤러)가 모두 리액터 코어를 물려받아
    //    워커 코어를 비워 둔다. 워커 스레드는 각자 워커 코어로 다시 고정한다.
    const cpu::Plan cpuPlan = cpu::plan(optionalEnv("PIN", "auto"),
                                        optionalEnv("REACTOR_CPU", ""),
                                        optionalEnv("WORKER_CPUS", ""));
    if (cpuPlan.pin && !cpu::pinThisThread(cpuPlan.reactorCpus)) {
        Log::warn("리액터 코어 고정 실패 — 고정 없이 진행");
    }
    Log::info("CPU: " + cpuPlan.describe());
    raiseFdLimit();

    const PoolSettings ps = PoolSettings::fromEnv(cpuPlan.workerCores);
    Log::info("Pool: " + ps.describe());

    // 1. 환경변수 로드 (가짜 API 실험에서는 외부 API 키가 필요 없다)
    const std::string seoulApiKey = ps.fakeApiMs ? optionalEnv("SEOUL_API_KEY", "")
                                                 : requireEnv("SEOUL_API_KEY");
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
    if (ps.fakeApiMs) {
        router.addClient(std::make_shared<FakeCongestionClient>(ps.fakeApiMs));
        Log::info("CongestionRouter ready (fake API " + std::to_string(ps.fakeApiMs) + "ms)");
    } else {
        router.addClient(std::make_shared<SeoulCityDataClient>(seoulApiKey));
        router.addClient(std::make_shared<DaeguTrafficClient>(daeguApiKey));
        Log::info("CongestionRouter ready (Seoul + Daegu registered)");
    }
    router.start(ps.asyncRouter ? CongestionRouter::Mode::Async : CongestionRouter::Mode::Sync,
                 cacheManager);

    // 3. 공공데이터 피더 (API 키가 없으면 끈다 — 가짜 API 실험)
    PublicDataFeeder feeder(densityEngine, seoulApiKey);

    // 4. 조회 큐·워커: 요청 종류별 ClassPool + 10ms 컨트롤러
    QueryService queryService(densityEngine, router, cacheManager);
    const auto process = [&queryService](Job& j, int64_t deqNs) { queryService.process(j, deqNs); };
    const auto expire  = [&queryService](Job& j) { queryService.expire(j); };
    const std::vector<int> workerCpus = cpuPlan.pin ? cpuPlan.workerCpus : std::vector<int>{};

    ControllerParams ctrlParams;
    auto classConfig = [&](double mu0) {
        PoolController::ClassConfig cc;
        cc.mu0        = mu0;
        cc.adaptive   = ps.adaptive;
        cc.dynamicCap = ps.dynamicCap;
        cc.fixedCap   = ps.fixedCap;
        cc.cMin       = ps.adaptive ? ps.cMin : ps.workers;
        cc.cMax       = ps.adaptive ? ps.cMax : ps.workers;
        return cc;
    };
    auto makePool = [&](const std::string& name, int nice, PoolController::ClassConfig& cc) {
        ClassPool::Options o;
        o.name          = name;
        o.threads       = ps.threads();
        o.initialActive = ps.initialActive();
        o.cpus          = workerCpus;
        o.nice          = nice;
        cc.pool         = nullptr;
        // 시작 직후 K: 실험 0 에서 잰 μ(MU0_*)로 계산 (활성 스레드 = initialActive)
        o.initialCap = cc.dynamicCap
                       ? std::clamp<size_t>(static_cast<size_t>(o.initialActive * cc.mu0
                                                                * ctrlParams.waitMaxSec),
                                            ctrlParams.kMin, ctrlParams.kMax)
                       : cc.fixedCap;
        auto p  = std::make_unique<ClassPool>(o, process, expire);
        cc.pool = p.get();
        return p;
    };

    std::vector<PoolController::ClassConfig> ctrlClasses;
    auto simpleCfg = classConfig(ps.mu0Simple);
    std::unique_ptr<ClassPool> simplePool =
            makePool(ps.split ? "SIMPLE" : "SHARED", 0, simpleCfg);
    ctrlClasses.push_back(simpleCfg);
    std::unique_ptr<ClassPool> complexPool;
    if (ps.split) {
        auto complexCfg = classConfig(ps.mu0Complex);
        complexPool = makePool("COMPLEX", ps.complexNice, complexCfg);
        ctrlClasses.push_back(complexCfg);
    }
    ClassPool& complexRef = complexPool ? *complexPool : *simplePool;

    PoolController controller(ctrlClasses, ctrlParams, ps.csvPath,
                              cpuPlan.pin ? cpuPlan.reactorCpus : std::vector<int>{});
    controller.start();
    if (!ps.csvPath.empty()) Log::info("Controller CSV: " + ps.csvPath);

    // 5. 밀도 엔진 청소 스레드
    std::thread cleanupThread;

    // 6. epoll 서버 생성
    EpollServer server(static_cast<uint16_t>(port),
                       densityEngine, zoneMapper, queryService, *simplePool, complexRef);
    g_server = &server;

    if (!seoulApiKey.empty()) feeder.start();

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
    controller.stop();
    // 워커가 router·cache·curl 을 쓰므로 그것들보다 먼저, curl_global_cleanup 전에 멈춘다
    complexPool.reset();
    simplePool.reset();
    g_cleanupRunning = false;
    if (cleanupThread.joinable()) cleanupThread.join();
    feeder.stop();
    router.stop();

    // densityEngine(먼저 선언)보다 redisClient(나중 선언)가 먼저 소멸하므로,
    // 소멸자에 맡기지 말고 여기서 writer 스레드를 명시적으로 중지한다.
    densityEngine.stopRedisPersistence();

    g_server = nullptr;
    curl_global_cleanup();
    return 0;
}