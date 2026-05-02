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

        // 1. 슬라이딩 윈도우에서 만료 이벤트 일괄 수집
        std::vector<ExpiredEvent> expired = m_slidingWindow.sweepExpired();
        if (expired.empty()) continue;

        // 2. 만료된 userId 목록 수집 (중복 제거)
        std::unordered_set<int> expiredUsers;
        for (const auto& e : expired) {
            expiredUsers.insert(e.userId);
        }

        // 3. 완전 이탈 사용자 처리
        //    sweepExpired() 후 deque가 빈 사용자 = windowSec 동안 이벤트 없음
        int removedCount = 0;
        for (int userId : expiredUsers) {
            if (m_slidingWindow.getActiveEventCount(userId) == 0) {
                // UserCountManager의 userZones에서 해당 userId의 zone 카운트를
                // 직접 보정: 현재 zone의 카운트 -1 후 userZones에서 제거
                //
                // UserCountManager에 removeUser() 함수가 없으므로
                // zoneId=-1 트릭 대신 아래처럼 새 함수를 추가하는 것을 권장:
                //
                //   void UserCountManager::removeUser(int userId) {
                //       std::unique_lock<std::mutex> lock(mtx);
                //       if (userZones.count(userId)) {
                //           int oldZone = userZones[userId];
                //           if (zoneCounts.count(oldZone) && zoneCounts[oldZone] > 0)
                //               zoneCounts[oldZone]--;
                //           userZones.erase(userId);
                //       }
                //   }
                //
                // 추가 후 아래 주석 해제:
                // m_userMgr.removeUser(userId);

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
