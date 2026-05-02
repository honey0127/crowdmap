#ifndef SLIDINGWINDOW_H
#define SLIDINGWINDOW_H

/**
 * @file SlidingWindow.h
 * @brief 슬라이딩 윈도우 - 오래된 위치 데이터 자동 제거

 * ■ 자료구조
 *   deque<TimestampedLocation>
 *     front = 가장 오래된 데이터
 *     back  = 가장 최신 데이터
 *   unordered_map<int userId, deque>
 *
 * ■ 시간복잡도
 *   addEvent()    : O(1) amortized
 *   sweepExpired(): O(n)  n = 전체 사용자 수
 */

#include <deque>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>

// 타임스탬프가 붙은 위치 이벤트
struct TimestampedLocation {
    int zoneId;
    std::chrono::steady_clock::time_point recordedAt;
};

// 만료된 이벤트 (DeadSessionSweeper가 UserCountManager 보정에 사용)
struct ExpiredEvent {
    int userId;
    int zoneId;
};

class SlidingWindow {
public:
    /**
     * @param windowSec  유효 데이터 유지 시간(초). 기본 300 = 5분
     * @param maxHistory 사용자당 최대 보관 이벤트 수 (메모리 상한 보조)
     */
    explicit SlidingWindow(int windowSec = 300, int maxHistory = 60);

    /**
     * @brief 새 위치 이벤트 추가 + 만료 항목 즉시 제거
     *
     * [호출 위치] main.cpp handleClient() 안, zoneId 계산 직후:
     *   int zoneId = zoneMapper.coordinateToZoneId(latitude, longitude);
     *   slidingWindow.addEvent(userId, zoneId);  ← 여기
     *   userCountManager.updateUserLocation(userId, zoneId);
     *
     * @return 이번 호출로 만료된 이벤트 목록 (없으면 빈 벡터)
     */
    std::vector<ExpiredEvent> addEvent(int userId, int zoneId);

    /**
     * @brief 전체 사용자 만료 항목 일괄 스캔 및 제거
     *
     * [호출 위치] DeadSessionSweeper::sweepLoop() 에서 주기적으로 호출
     *
     * @return 만료된 이벤트 전체 목록
     */
    std::vector<ExpiredEvent> sweepExpired();

    /**
     * @brief 특정 사용자 이력 전체 삭제
     *
     * [호출 위치] DeadSessionSweeper가 완전 이탈 사용자 정리 시
     */
    void removeUser(int userId);

    // 특정 사용자의 현재 유효 이벤트 수 (디버깅/통계용)
    int getActiveEventCount(int userId);

    // 추적 중인 전체 사용자 수
    int getTotalTrackedUsers();

private:
    int m_windowSec;
    int m_maxHistory;
    // userId → 위치 이벤트 deque (front=오래됨, back=최신)
    std::unordered_map<int, std::deque<TimestampedLocation>> m_history;
    std::mutex m_mtx;
};

#endif // SLIDINGWINDOW_H
