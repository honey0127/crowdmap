#ifndef CLASS_POOL_H
#define CLASS_POOL_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ChunkQueue.h"

// 단조 시계(ns). 큐 투입·꺼냄·완료 시각을 모두 이 시계로 잰다.
int64_t monoNowNs();

/**
 * @brief 조회 작업 1건 (힙 할당 없는 고정 크기 — std::function 대신)
 */
struct Job {
    int      fd     = -1;    // dup 된 소켓. 작업이 소유하며 응답 후 close 한다.
    int      zoneId = -1;
    double   lat    = 0.0;
    double   lon    = 0.0;
    uint32_t workUs = 0;     // 실험 빌드: 합성 CPU 작업량(µs). 0 = SIMPLE
    uint64_t reqId  = 0;     // 실험 빌드: 응답 매칭용 요청 번호 (0 = 없음)
    int64_t  tEnqNs = 0;     // 큐 투입 시각
};

/**
 * @brief 컨트롤러가 10ms 마다 읽는 누적 카운터 스냅숏
 */
struct PoolSnapshot {
    uint64_t offered  = 0;   // 도착(받음 + shed)
    uint64_t shed     = 0;   // 상한 초과로 받지 않음
    uint64_t dequeued = 0;   // 큐에서 꺼냄(처리 + 마감 초과)
    uint64_t expired  = 0;   // 꺼냈을 때 이미 마감(2초) 초과 → 옛 캐시 응답
    uint64_t done     = 0;   // 처리 완료
    double   svcSum   = 0;   // 처리 시간 합(초, 벽시계)
    double   svcSqSum = 0;   // 처리 시간 제곱 합(초²) → Cs²
    double   waitSum  = 0;   // 대기 시간 합(초) → Little 법칙 검증
    size_t   qlen     = 0;   // 대기 중(처리 중 제외)
    size_t   chunks   = 0;   // 큐 메모리 블록 수
    int      active   = 0;   // 깨어 있는 스레드 수 c
    size_t   cap      = 0;   // 입장 상한 K
};

/**
 * @brief 요청 종류(SIMPLE/COMPLEX) 하나를 맡는 큐 + 스레드 풀
 *
 *  - 입장 상한 K(t): 컨트롤러가 setCap() 으로 갱신. Q ≥ K 면 tryEnqueue 가 false
 *    → 호출자가 옛 캐시로 즉시 응답(shed). 이미 들어온 요청은 K 가 줄어도 버리지 않는다.
 *  - 활성 스레드 c: setActive() 로 조절. 스레드를 종료하지 않고 재운다(park).
 *    자는 동안 CPU 0, 다시 깨우는 데 수십 µs.
 *  - 꺼낼 때 마감 검사: 투입 후 deadline 을 넘긴 요청은 처리하지 않고 expire 콜백
 *    (옛 캐시 응답)으로 넘긴다. 클라이언트가 어차피 타임아웃 낼 요청에 CPU 를 쓰지 않는다.
 *  - 메모리: ChunkQueue (1,024칸 블록을 필요할 때 붙이고 비면 반납).
 */
class ClassPool {
public:
    // process: 작업 처리 + 응답 송신. deqNs = 꺼낸 시각.
    using ProcessFn = std::function<void(Job&, int64_t deqNs)>;
    // expire: 마감 초과 작업에 대한 대체 응답(옛 캐시).
    using ExpireFn  = std::function<void(Job&)>;

    struct Options {
        std::string      name          = "pool";
        int              threads       = 1;       // 만들어 둘 스레드 수(= c 의 상한)
        int              initialActive = 1;
        size_t           initialCap    = 10'000;
        std::vector<int> cpus;                    // 워커 코어 고정(비우면 고정 안 함)
        int              nice          = 0;       // COMPLEX 는 +5 → SIMPLE 이 코어를 우선 차지
        int64_t          deadlineNs    = 2'000'000'000LL;
    };

    ClassPool(Options opt, ProcessFn process, ExpireFn expire);
    ~ClassPool();

    ClassPool(const ClassPool&) = delete;
    ClassPool& operator=(const ClassPool&) = delete;

    // 리액터 스레드에서 호출. 상한 초과면 false (job 은 그대로 남아 호출자가 정리).
    bool tryEnqueue(Job& job);

    void setActive(int n);       // 1..threads 로 잘림
    void setCap(size_t k);

    int    threads() const { return static_cast<int>(workers_.size()); }
    int    active()  const { return active_.load(std::memory_order_relaxed); }
    size_t cap()     const { return cap_.load(std::memory_order_relaxed); }
    size_t pending() const { return qlen_.load(std::memory_order_relaxed); }
    const std::string& name() const { return opt_.name; }

    PoolSnapshot snapshot();

private:
    struct alignas(64) WorkerStats {        // 스레드별(쓰는 쪽 1개) → 원자적 RMW 불필요
        std::atomic<uint64_t> done{0};
        std::atomic<double>   svc{0.0};
        std::atomic<double>   svcSq{0.0};
        std::atomic<double>   wait{0.0};
    };

    void workerLoop(int idx);

    Options   opt_;
    ProcessFn process_;
    ExpireFn  expire_;

    std::mutex              m_;
    std::condition_variable workCv_;   // 깨어 있는 스레드가 일을 기다림
    std::condition_variable parkCv_;   // 재워 둔 스레드가 깨워 주기를 기다림
    ChunkQueue<Job>         q_;
    bool                    stop_ = false;

    // m_ 아래에서만 바뀌는 누적 카운터
    uint64_t offered_  = 0;
    uint64_t shed_     = 0;
    uint64_t dequeued_ = 0;

    std::atomic<int>      active_{1};
    std::atomic<size_t>   cap_{10'000};
    std::atomic<size_t>   qlen_{0};
    std::atomic<uint64_t> expired_{0};

    std::unique_ptr<WorkerStats[]> stats_;
    std::vector<std::thread>       workers_;
};

#endif // CLASS_POOL_H
