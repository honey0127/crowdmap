#include "SlidingWindow.h"

SlidingWindow::SlidingWindow(int windowSec, int maxHistory)
        : m_windowSec(windowSec), m_maxHistory(maxHistory) {}

std::vector<ExpiredEvent> SlidingWindow::addEvent(int userId, int zoneId) {
    std::unique_lock<std::mutex> lock(m_mtx);
    auto now = std::chrono::steady_clock::now();

    auto& dq = m_history[userId];

    dq.push_back({zoneId, now}); // 최신 이벤트 뒤에 추가
    // 메모리 상환 : 60개 초과 시 오래된 것 제거
    while ((int)dq.size() > m_maxHistory) {
        dq.pop_front();
    }
    // 5분 지난 이벤트 즉시 만료 처리
    std::vector<ExpiredEvent> expired;
    //deque의 front가 항상 가장 오래됐다는 보장이 있으므로 앞에서부터만 체크해서 O(만료된 수)만 순회
    while (!dq.empty()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - dq.front().recordedAt).count(); // front가 살아있으면 뒤도 다 살아있음(정렬 보장)
        if (age <= m_windowSec) break;

        expired.push_back({userId, dq.front().zoneId});
        dq.pop_front();
    }

    return expired;
}

bool SlidingWindow::removeUserIfEmpty(int userId) {
    std::unique_lock<std::mutex> lock(m_mtx);  // 확인 + 삭제가 하나의 락 안에서
    auto it = m_history.find(userId);
    if (it == m_history.end() || !it->second.empty()) return false;
    m_history.erase(it);
    return true;
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
