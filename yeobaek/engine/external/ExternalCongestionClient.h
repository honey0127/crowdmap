#pragma once
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// [이식+확장] CrowdMap 의 ExternalCongestionClient 를 여백(engine)으로 이식하고,
//            도착 시점(T)의 "예보" 를 뽑기 위한 시간축을 확장한다. (절차서 결정 A)
//
// 확장 원칙:
//   - 기존 실시간 계약(getCongestion / covers)은 그대로 둔다.
//   - 예보는 오버라이드가 아니라 "인터페이스 확장" 으로 추가한다.
//   - base 에 default 구현(valid=false)을 두어, 예보를 지원하지 않는 실시간 전용
//     클라이언트(예: Daegu)는 코드 변경 없이 그대로 동작한다.
// ─────────────────────────────────────────────────────────────────────────────

struct ExternalCongestionResult {
    int level;          // 1:여유 2:보통 3:약간붐빔(혼잡) 4:붐빔(매우혼잡)
    std::string source; // "seoul", "public", "internal"
    bool valid;
};

// [신규] 도착 시점 예보 결과. level 은 실시간과 동일한 1~4 척도를 유지한다.
//        fcstUnixTime 은 실제로 채택된 예보 시각(가장 가까운 FCST_TIME)의 epoch(초).
struct ForecastResult {
    int level;          // 1~4 (실시간과 동일 척도)
    long fcstUnixTime;  // 채택된 예보 시각 (epoch seconds), 0 = 미상
    int ppltnMin;       // 예상 최소 인구
    int ppltnMax;       // 예상 최대 인구
    bool valid;         // 예보를 실제로 얻었는지
};

class ExternalCongestionClient {
public:
    virtual ~ExternalCongestionClient() = default;

    // ── 기존 실시간 계약 ──
    virtual ExternalCongestionResult getCongestion(double lat, double lng) = 0;
    virtual bool covers(double lat, double lng) = 0; // 이 API 가 해당 좌표를 지원하는지

    // ── [확장] 예보 계약 (결정 A) ──
    // arrivalUnixTime(도착 예정 epoch seconds)의 예보 혼잡도.
    // 예보 미지원 클라이언트는 이 default 를 그대로 사용한다(valid=false).
    virtual ForecastResult getForecast(double lat, double lng, long arrivalUnixTime) {
        (void)lat; (void)lng; (void)arrivalUnixTime;
        return {1, 0, 0, 0, false}; // 예보 미지원 클라이언트의 기본값
    }
};
