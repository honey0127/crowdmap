#ifndef GRID_ZONE_H
#define GRID_ZONE_H

#include <deque>
#include <chrono>
#include <cstdint>

/**
 * @brief 그리드 단위의 세부 구역 데이터
 * 슬라이딩 윈도우(5분) 기반으로 유효한 사용자 이벤트를 관리합니다.
 */
struct UserEvent {
    int32_t userId;
    std::chrono::steady_clock::time_point timestamp;
};

class GridZone {
public:
    /**
     * @brief 사용자 위치 이벤트 기록 및 만료 데이터 정리
     * @param userId 사용자 ID
     * @param now 현재 시간 (성능을 위해 외부에서 전달)
     */
    void update(int32_t userId, std::chrono::steady_clock::time_point now) {
        m_events.push_back({userId, now});
    }

    /**
     * @brief 300초(5분)가 지난 데이터 제거
     */
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

    /**
     * @brief 현재 구역의 유효 사용자 밀도 반환
     */
    size_t getDensity() const {
        return m_events.size();
    }

private:
    std::deque<UserEvent> m_events;
};

#endif // GRID_ZONE_H
