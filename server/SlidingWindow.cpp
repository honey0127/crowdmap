#include "SlidingWindow.h"

SlidingWindow::SlidingWindow(int windowSec, int maxHistory)
        : m_windowSec(windowSec), m_maxHistory(maxHistory) {}

std::vector<ExpiredEvent> SlidingWindow::addEvent(int userId, int zoneId) {
    std::unique_lock<std::mutex> lock(m_mtx);
    auto now = std::chrono::steady_clock::now();

    auto& dq = m_history[userId];

    dq.push_back({zoneId, now});

    while ((int)dq.size() > m_maxHistory) {
        dq.pop_front();
    }

    std::vector<ExpiredEvent> expired;
    while (!dq.empty()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - dq.front().recordedAt).count();
        if (age <= m_windowSec) break;

        expired.push_back({userId, dq.front().zoneId});
        dq.pop_front();
    }

    return expired;
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
