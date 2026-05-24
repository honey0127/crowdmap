#include "DeadSessionSweeper.h"
#include "Logger.h"

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
    Log::info("[DeadSessionSweeper] 시작 (interval="
              + std::to_string(SWEEP_INTERVAL_SEC) + "s, timeout="
              + std::to_string(m_timeoutSec) + "s)");
}

void DeadSessionSweeper::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void DeadSessionSweeper::sweepLoop() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(SWEEP_INTERVAL_SEC));
        if (!m_running) break; // 술립 중 종료 신호 처리
        // slidingwindow에서 5분 지난 이벤트 전부 수거
        std::vector<ExpiredEvent> expired = m_slidingWindow.sweepExpired();
        if (expired.empty()) continue; // 없으면 바로 다음 주기

        std::unordered_set<int> expiredUsers;
        for (const auto& e : expired) {
            expiredUsers.insert(e.userId);
        }

        int removedCount = 0;
        for (int userId : expiredUsers) {
            if (m_slidingWindow.removeUserIfEmpty(userId)) {
                m_userMgr.removeUser(userId);
                ++removedCount;
            }
        }

        if (removedCount > 0) {
            Log::info("[DeadSessionSweeper] 고스트 유저 " + std::to_string(removedCount)
                      + "명 정리 완료. (만료 이벤트 총 " + std::to_string(expired.size()) + "건)");
        }
    }
} // unordered_set으로 중복 userid를 합쳐도 같은 유저를 여러 번 처리하지 않음
// getActiveEventCount 재확인이 중요하다. why? -> sweepExpired 후 1-2ms 사이에 해당 유저의 새 패킷이 도착할 수 있어서이다.
