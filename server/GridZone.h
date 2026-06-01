#ifndef GRID_ZONE_H
#define GRID_ZONE_H

#include <deque>
#include <chrono>
#include <cstdint>
#include <algorithm>

struct UserEvent {
    int32_t userId;
    std::chrono::steady_clock::time_point timestamp;
};

// BLE 스캔 결과 이벤트 (단말기가 감지한 주변 BLE 기기 수)
struct BleEvent {
    int bleCount;
    std::chrono::steady_clock::time_point timestamp;
};

class GridZone {
public:
    void update(int32_t userId, std::chrono::steady_clock::time_point now) {
        m_events.push_back({userId, now});
        cleanup(now);
    }

    // BLE 감지 기기 수를 5분 슬라이딩 윈도우에 기록
    void updateBle(int bleCount, std::chrono::steady_clock::time_point now) {
        m_bleEvents.push_back({bleCount, now});
        cleanupBle(now);
    }

    void cleanup(std::chrono::steady_clock::time_point now) {
        while (!m_events.empty()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - m_events.front().timestamp).count();
            if (elapsed > 300) {
                m_events.pop_front();
            } else {
                break;
            }
        }
    }

    void cleanupBle(std::chrono::steady_clock::time_point now) {
        while (!m_bleEvents.empty()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - m_bleEvents.front().timestamp).count();
            if (elapsed > 300) {
                m_bleEvents.pop_front();
            } else {
                break;
            }
        }
    }

    size_t getDensity() const {
        return m_events.size();
    }

    // 최근 BLE 스캔 이벤트 중 최대값 반환 (없으면 0)
    int getMaxBleCount() const {
        int maxBle = 0;
        for (const auto& ev : m_bleEvents) {
            maxBle = std::max(maxBle, ev.bleCount);
        }
        return maxBle;
    }

private:
    std::deque<UserEvent> m_events;
    std::deque<BleEvent>  m_bleEvents;
};

#endif // GRID_ZONE_H
