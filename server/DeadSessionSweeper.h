#ifndef DEADSESSIONSWEEPER_H
#define DEADSESSIONSWEEPER_H
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_set>

#include "UserCountManager.h"
#include "SlidingWindow.h"

class DeadSessionSweeper {
public:
    static constexpr int SWEEP_INTERVAL_SEC = 10; // 10초마다 스캔

    DeadSessionSweeper(UserCountManager& userMgr,
                       SlidingWindow&    slidingWindow,
                       int               timeoutSec);

    ~DeadSessionSweeper();

    void start(); // 백그라운드 루프 시작
    void stop();  // 루프 종료

private:
    void sweepLoop();

    UserCountManager& m_userMgr;
    SlidingWindow&    m_slidingWindow;
    int               m_timeoutSec;
    std::atomic<bool> m_running; // volatile 대신 atomic (메모리 가시성 보장)
    std::thread       m_thread; // 완전 독립 백그라운드 스레드
};

#endif // DEADSESSIONSWEEPER_H
