// 큐·컨트롤러 단위 테스트 (외부 프레임워크 없음)
//   빌드: cmake --build build --target queue_tests && ./build/queue_tests
#include "ChunkQueue.h"
#include "ClassPool.h"
#include "PoolController.h"
#include "QueueMath.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (cond) {                                                                   \
            ++g_pass;                                                                 \
        } else {                                                                      \
            ++g_fail;                                                                 \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                         \
    do {                                                                              \
        const double _a = (a), _b = (b);                                              \
        if (std::fabs(_a - _b) <= (tol)) {                                            \
            ++g_pass;                                                                 \
        } else {                                                                      \
            ++g_fail;                                                                 \
            std::fprintf(stderr, "FAIL %s:%d  %s = %.6g, 기대 %.6g ± %.3g\n",          \
                         __FILE__, __LINE__, #a, _a, _b, static_cast<double>(tol));   \
        }                                                                             \
    } while (0)

using namespace std::chrono_literals;

// ── 1. 대기행렬 계산: 설계 문서 수치 재현 ─────────────────────────────────
static void testQueueMath() {
    // M/M/c, ρ = 0.9 대기 확률 (피드백 정리 4.1 표)
    const int    cs[]  = {1, 2, 4, 8, 16, 32};
    const double pw[]  = {0.900, 0.853, 0.788, 0.702, 0.591, 0.458};
    for (int i = 0; i < 6; ++i) CHECK_NEAR(qmath::erlangC(cs[i], 0.9 * cs[i]), pw[i], 0.0015);

    // K_floor: ε = 1e-4, ρ = 0.9. 정확식 87(c=1), 86(c=2·3) / 근사식 82(지수), 41(고정)
    CHECK(qmath::kFloorMMc(1, 0.9, 1e-4) == 87);
    CHECK(qmath::kFloorMMc(2, 1.8, 1e-4) == 86);
    CHECK(qmath::kFloorMMc(3, 2.7, 1e-4) == 86);
    CHECK(std::ceil(qmath::kFloor(0.9, 1.0, 1.0, 1e-4)) == 82);
    CHECK(std::ceil(qmath::kFloor(0.9, 1.0, 0.0, 1e-4)) == 41);
    CHECK(qmath::kFloor(1.2, 1.0, 1.0, 1e-4) >= qmath::BIG);   // 포화
    CHECK(qmath::kFloor(0.0, 1.0, 1.0, 1e-4) == 0.0);

    // 실험 C 예측: COMPLEX(10ms 지수) 초당 50/150/200건 → Erlang C 최소 스레드 2/3/3
    CHECK(qmath::minServers(50, 100, 0.05, 0.01, 0.9, 1.0, 1.0, 8) == 2);
    CHECK(qmath::minServers(150, 100, 0.05, 0.01, 0.9, 1.0, 1.0, 8) == 3);
    CHECK(qmath::minServers(200, 100, 0.05, 0.01, 0.9, 1.0, 1.0, 8) == 3);
    // 같은 조건의 '단순 계산'(ρ ≤ 0.9 만) 1/2/3 과 다름을 확인
    CHECK(static_cast<int>(std::ceil(50.0 / 100 / 0.9)) == 1);
    CHECK(static_cast<int>(std::ceil(150.0 / 100 / 0.9)) == 2);
    // 고정 서비스(Cs² = 0)면 꼬리가 짧아 50건/s 는 1개로 충분
    CHECK(qmath::minServers(50, 100, 0.05, 0.01, 0.9, 1.0, 0.0, 8) == 1);
    // 상한에 걸리면 cMax
    CHECK(qmath::minServers(1000, 100, 0.05, 0.01, 0.9, 1.0, 1.0, 3) == 3);
    // M/M/c 꼬리 정확식: c=1, λ=50, μ=100, t=50ms → 0.5·e^−2.5
    CHECK_NEAR(qmath::pWaitGt(1, 50, 100, 0.05, 1, 1), 0.5 * std::exp(-2.5), 1e-9);
}

// ── 2. ChunkQueue ─────────────────────────────────────────────────────────
static void testChunkQueue() {
    ChunkQueue<int, 4> q;
    CHECK(q.empty() && q.chunks() == 0);

    for (int i = 0; i < 10; ++i) {
        int v = i;
        q.push(std::move(v));
    }
    CHECK(q.size() == 10);
    CHECK(q.chunks() == 3);   // 4 + 4 + 2

    int out = -1;
    for (int i = 0; i < 5; ++i) {
        CHECK(q.pop(out));
        CHECK(out == i);   // FIFO
    }
    // 첫 블록이 비어 여분으로 보관: 블록 수 그대로
    CHECK(q.chunks() == 3);
    for (int i = 10; i < 14; ++i) {   // 꼬리에 새 블록이 필요하면 여분을 재사용
        int v = i;
        q.push(std::move(v));
    }
    CHECK(q.chunks() == 3);
    for (int i = 5; i < 14; ++i) {
        CHECK(q.pop(out));
        CHECK(out == i);
    }
    CHECK(q.empty());
    CHECK(!q.pop(out));
    CHECK(q.chunks() == 1);   // 비면 여분까지 반납하고 1개만 남김

    // 블록 경계에서 왕복해도 할당 수가 늘지 않음
    for (int r = 0; r < 100; ++r) {
        for (int i = 0; i < 5; ++i) {
            int v = i;
            q.push(std::move(v));
        }
        for (int i = 0; i < 4; ++i) q.pop(out);
        CHECK(q.chunks() <= 3);
        q.pop(out);
    }
}

// 테스트용 관문: open() 전까지 워커를 붙잡아 둔다
struct Gate {
    std::mutex              m;
    std::condition_variable cv;
    bool                    opened = false;
    void wait() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return opened; });
    }
    void open() {
        {
            std::lock_guard<std::mutex> lk(m);
            opened = true;
        }
        cv.notify_all();
    }
};

static bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

// ── 3. ClassPool: 상한·마감·재우기 ─────────────────────────────────────────
static void testClassPoolCapAndDeadline() {
    Gate               gate;
    std::atomic<int>   processed{0};
    std::atomic<int>   expired{0};
    ClassPool::Options o;
    o.name          = "T";
    o.threads       = 1;
    o.initialActive = 1;
    o.initialCap    = 3;
    o.deadlineNs    = 20'000'000;   // 20ms
    ClassPool pool(o,
                   [&](Job&, int64_t) {
                       gate.wait();
                       ++processed;
                   },
                   [&](Job&) { ++expired; });

    Job j;
    CHECK(pool.tryEnqueue(j));                              // 워커가 꺼내 관문에서 대기
    CHECK(waitFor([&] { return pool.pending() == 0; }, 500ms));
    for (int i = 0; i < 3; ++i) {
        Job k;
        CHECK(pool.tryEnqueue(k));                          // 큐 3개 = 상한
    }
    Job over;
    CHECK(!pool.tryEnqueue(over));                          // Q ≥ K → shed
    pool.setCap(5);
    Job more;
    CHECK(pool.tryEnqueue(more));                           // 상한을 올리면 다시 받음

    std::this_thread::sleep_for(40ms);                      // 큐에 있는 4개가 마감(20ms) 초과
    gate.open();
    CHECK(waitFor([&] { return processed + expired == 5; }, 1000ms));
    CHECK(processed == 1);
    CHECK(expired == 4);

    const PoolSnapshot s = pool.snapshot();
    CHECK(s.offered == 6);
    CHECK(s.shed == 1);
    CHECK(s.dequeued == 5);
    CHECK(s.expired == 4);
    CHECK(s.done == 1);
    CHECK(s.qlen == 0);
}

static void testClassPoolParking() {
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    std::atomic<int> processed{0};
    ClassPool::Options o;
    o.name          = "P";
    o.threads       = 3;
    o.initialActive = 1;
    o.initialCap    = 1000;
    ClassPool pool(o,
                   [&](Job&, int64_t) {
                       const int now = ++running;
                       int p = peak.load();
                       while (now > p && !peak.compare_exchange_weak(p, now)) {}
                       std::this_thread::sleep_for(2ms);
                       --running;
                       ++processed;
                   },
                   [](Job&) {});

    for (int i = 0; i < 30; ++i) {
        Job j;
        pool.tryEnqueue(j);
    }
    CHECK(waitFor([&] { return processed == 30; }, 2000ms));
    CHECK(peak == 1);                 // 재워 둔 스레드는 일하지 않는다

    peak = 0;
    pool.setActive(3);
    CHECK(pool.active() == 3);
    for (int i = 0; i < 60; ++i) {
        Job j;
        pool.tryEnqueue(j);
    }
    CHECK(waitFor([&] { return processed == 90; }, 2000ms));
    CHECK(peak == 3);                 // 깨우면 3개가 동시에 처리

    pool.setActive(1);
    peak = 0;
    for (int i = 0; i < 30; ++i) {
        Job j;
        pool.tryEnqueue(j);
    }
    CHECK(waitFor([&] { return processed == 120; }, 2000ms));
    CHECK(peak == 1);                 // 다시 재움

    pool.setActive(99);
    CHECK(pool.active() == 3);        // 스레드 수로 잘림
    pool.setActive(0);
    CHECK(pool.active() == 1);
}

// ── 4. 컨트롤러 ──────────────────────────────────────────────────────────
static void spinWallUs(int us) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < until) {}
}

// CSV 의 마지막 n 줄에서 열 이름으로 값 읽기
static std::vector<std::vector<std::string>> readCsv(const std::string& path,
                                                     std::vector<std::string>& header) {
    std::ifstream f(path);
    std::string line;
    std::vector<std::vector<std::string>> rows;
    bool first = true;
    while (std::getline(f, line)) {
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) cols.push_back(c);
        if (first) {
            header = cols;
            first  = false;
        } else {
            rows.push_back(cols);
        }
    }
    return rows;
}

static int colIndex(const std::vector<std::string>& header, const std::string& name) {
    for (size_t i = 0; i < header.size(); ++i)
        if (header[i] == name) return static_cast<int>(i);
    return -1;
}

// 처리 1ms 작업을 스레드 1개 용량(1,000/s)의 2배로 넣으면 c 가 늘고, 멈추면 1로 돌아온다
static void testControllerScaleUpDown() {
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw < 4) {
        std::printf("SKIP testControllerScaleUpDown (코어 %u < 4)\n", hw);
        return;
    }
    ClassPool::Options o;
    o.name          = "SIMPLE";
    o.threads       = 3;
    o.initialActive = 1;
    o.initialCap    = 1000;
    ClassPool pool(o, [](Job&, int64_t) { spinWallUs(1000); }, [](Job&) {});

    PoolController::ClassConfig cc;
    cc.pool = &pool;
    cc.mu0  = 1000.0;
    cc.cMin = 1;
    cc.cMax = 3;
    const std::string csv = "/tmp/queue_tests_ctrl_" + std::to_string(getpid()) + ".csv";
    PoolController ctrl({cc}, ControllerParams{}, csv, {});
    ctrl.start();

    std::atomic<bool> feeding{true};
    std::thread feeder([&] {
        // 초당 2,000건 (1ms 마다 2건)
        auto next = std::chrono::steady_clock::now();
        while (feeding) {
            for (int i = 0; i < 2; ++i) {
                Job j;
                pool.tryEnqueue(j);
            }
            next += 1ms;
            std::this_thread::sleep_until(next);
        }
    });

    const bool scaledUp = waitFor([&] { return pool.active() >= 2; }, 1000ms);
    CHECK(scaledUp);
    std::this_thread::sleep_for(1500ms);
    const size_t qDuring = pool.pending();
    // 적응 후에는 대기가 목표(50ms ≈ 2·1,000·0.05 = 100건) 근처 이하
    CHECK(qDuring < 400);
    feeding = false;
    feeder.join();

    // 멈추면 200ms 마다 1개씩 줄어 1로
    const bool scaledDown = waitFor([&] { return pool.active() == 1; }, 3000ms);
    CHECK(scaledDown);
    ctrl.stop();

    std::vector<std::string> header;
    const auto rows = readCsv(csv, header);
    CHECK(rows.size() > 200);
    const int iK = colIndex(header, "k"), iC = colIndex(header, "c"), iMu = colIndex(header, "mu");
    CHECK(iK >= 0 && iC >= 0 && iMu >= 0);
    if (!rows.empty() && iK >= 0 && iMu >= 0 && iC >= 0) {
        // K = clamp(c·μ̂·1s, 64, 2^17), μ̂ ≈ 1,000/s
        const auto& last = rows.back();
        const double mu  = std::stod(last[iMu]);
        CHECK_NEAR(mu, 1000.0, 150.0);
        CHECK_NEAR(std::stod(last[iK]), std::max(64.0, std::stoi(last[iC]) * mu), 2.0);
    }
    std::remove(csv.c_str());
}

// 워커를 논리 CPU 1개에 모두 묶으면 스레드를 늘려도 처리량이 늘지 않는다 → c_max 가 깎인다
static void testControllerNoFreeCore() {
    if (std::thread::hardware_concurrency() < 2) {
        std::printf("SKIP testControllerNoFreeCore\n");
        return;
    }
    ClassPool::Options o;
    o.name          = "ONECPU";
    o.threads       = 3;
    o.initialActive = 1;
    o.initialCap    = 1000;
    o.cpus          = {1};
    // CPU 시간 기준 0.5ms 작업 (같은 코어를 나눠 쓰면 벽시계 처리 시간이 늘어난다)
    ClassPool pool(o, [](Job&, int64_t) {
        timespec a, b;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
        do { clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b); }
        while ((b.tv_sec - a.tv_sec) * 1'000'000'000LL + (b.tv_nsec - a.tv_nsec) < 500'000);
    }, [](Job&) {});

    PoolController::ClassConfig cc;
    cc.pool = &pool;
    cc.mu0  = 2000.0;
    cc.cMin = 1;
    cc.cMax = 3;
    const std::string csv = "/tmp/queue_tests_onecpu_" + std::to_string(getpid()) + ".csv";
    PoolController ctrl({cc}, ControllerParams{}, csv, {});
    ctrl.start();

    std::atomic<bool> feeding{true};
    std::thread feeder([&] {
        auto next = std::chrono::steady_clock::now();
        while (feeding) {   // 초당 4,000건: 코어 1개 용량(2,000/s)의 2배
            for (int i = 0; i < 4; ++i) {
                Job j;
                pool.tryEnqueue(j);
            }
            next += 1ms;
            std::this_thread::sleep_until(next);
        }
    });
    std::this_thread::sleep_for(1500ms);
    feeding = false;
    feeder.join();
    ctrl.stop();

    std::vector<std::string> header;
    const auto rows = readCsv(csv, header);
    const int  iCm  = colIndex(header, "c_max");
    bool cut = false;
    for (const auto& r : rows)
        if (iCm >= 0 && std::stoi(r[iCm]) < 3) cut = true;
    CHECK(cut);
    std::remove(csv.c_str());
}

int main() {
    testQueueMath();
    testChunkQueue();
    testClassPoolCapAndDeadline();
    testClassPoolParking();
    testControllerScaleUpDown();
    testControllerNoFreeCore();
    std::printf("queue_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
