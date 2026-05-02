#include "SlidingWindow.h"

SlidingWindow::SlidingWindow(int windowSec, int maxHistory)
        : m_windowSec(windowSec), m_maxHistory(maxHistory) {}

std::vector<ExpiredEvent> SlidingWindow::addEvent(int userId, int zoneId) {
    std::unique_lock<std::mutex> lock(m_mtx);

    // 2. maxHistory 초과 시 가장 오래된 것 제거 (메모리 상한 보조)
    while ((int)dq.size() > m_maxHistory) {
        dq.pop_front();
    }

    // 3. windowSec 초과 항목을 deque 앞에서 순서대로 제거 (슬라이딩 윈도우 핵심)
    //    deque는 앞=오래됨이므로 앞에서부터 체크
    //    앞이 유효하면 뒤는 무조건 유효 → break
    std::vector<ExpiredEvent> expired;
    while (!dq.empty()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - dq.front().recordedAt).count();
        if (age <= m_windowSec) break;

        expired.push_back({userId, dq.front().zoneId});
        dq.pop_front();
    }

    return expired; // 대부분의 경우 빈 벡터
}

std::vector<ExpiredEvent> SlidingWindow::sweepExpired() {
    std::unique_lock<std::mutex> lock(m_mtx);

    auto now = std::chrono::steady_clock::now();
    std::vector<ExpiredEvent> allExpired;

    for (auto& [userId, dq] : m_history) {
        while (!dq.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - dq.front().recordedAt).count();
            if (age <= m_windowSec) break;

            allExpired.push_back({userId, dq.front().zoneId});
            dq.pop_front();
        }
    }

    // deque가 완전히 비어버린 사용자 엔트리 정리
    for (auto it = m_history.begin(); it != m_history.end(); ) {
        it = it->second.empty() ? m_history.erase(it) : std::next(it);
    }

    return allExpired;
}

void SlidingWindow::removeUser(int userId) {
    std::unique_lock<std::mutex> lock(m_mtx);
    m_history.erase(userId);
}

int SlidingWindow::getActiveEventCount(int userId) {
    std::unique_lock<std::mutex> lock(m_mtx);
    auto it = m_history.find(userId);
    if (it == m_history.end()) return 0;
    return static_cast<int>(it->second.size());
}

int SlidingWindow::getTotalTrackedUsers() {
    std::unique_lock<std::mutex> lock(m_mtx);
    return static_cast<int>(m_history.size());
}
