#include "DeadSessionSweeper.h"

DeadSessionSweeper::DeadSessionSweeper(UserCountManager& userMgr,
                                       SlidingWindow&    slidingWindow,
                                       int               timeoutSec)
        : m_userMgr(userMgr),
          m_slidingWindow(slidingWindow),
          m_timeoutSec(timeoutSec),
          m_running(false) {}

DeadSessionSweeper::~DeadSessionSweeper() {
    stop();
}

void DeadSessionSweeper::start() {
    m_running = true;
    m_thread  = std::thread([this] { sweepLoop(); });
    std::cout << "[DeadSessionSweeper] 시작 (interval="
              << SWEEP_INTERVAL_SEC << "s, timeout="
              << m_timeoutSec << "s)\n";
}

void DeadSessionSweeper::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void DeadSessionSweeper::sweepLoop() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(SWEEP_INTERVAL_SEC));
        if (!m_running) break;

        std::vector<ExpiredEvent> expired = m_slidingWindow.sweepExpired();
        if (expired.empty()) continue;

        std::unordered_set<int> expiredUsers;
        for (const auto& e : expired) {
            expiredUsers.insert(e.userId);
        }

        int removedCount = 0;
        for (int userId : expiredUsers) {
            if (m_slidingWindow.getActiveEventCount(userId) == 0) {
                m_userMgr.removeUser(userId);
                m_slidingWindow.removeUser(userId);
                ++removedCount;
            }
        }

        if (removedCount > 0) {
            std::cout << "[DeadSessionSweeper] 고스트 유저 " << removedCount
                      << "명 정리 완료. (만료 이벤트 총 "
                      << expired.size() << "건)\n";
        }
    }
}
