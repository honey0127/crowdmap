#pragma once
#include "SeoulCityDataClient.h"
#include <ctime>
#include <climits>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// [신규] 서울 도시데이터 FCST_PPLTN(도착시점 예보) 클라이언트 (절차서 Phase 2-1)
//
// citydata_ppltn 응답 하나에 실시간(AREA_CONGEST_LVL)과 예보(FCST_PPLTN[])가
// 함께 담겨오므로, 단일 호출로 아래를 모두 처리한다:
//   1) arrivalTs 에 가장 가까운 FCST_TIME 항목 선택        → 예보 (fcstUnixTime>0)
//   2) FCST_PPLTN 부재 시 같은 payload 의 실시간 레벨로 대체 → realtime fallback
//                                                            (fcstUnixTime==0, valid=true)
//   3) 둘 다 없으면 valid=false  → 상위(ForecastProvider)가 인근 장소로 재시도
//
// getForecastByArea() 가 1차 기본형이고, base 의 getForecast(lat,lng,ts)(결정 A)는
// findNearestArea 로 area 를 구해 이 함수로 위임한다.
// ─────────────────────────────────────────────────────────────────────────────
class SeoulCityDataForecastClient : public SeoulCityDataClient {
public:
    using SeoulCityDataClient::SeoulCityDataClient;

    // 결정 A 계약 구현: 좌표 → 최근접 예보지점 → area 기반 예보.
    ForecastResult getForecast(double lat, double lng, long arrivalUnixTime) override {
        auto na = nearestArea(lat, lng);
        if (!na.found) return {1, 0, 0, 0, false};
        return getForecastByArea(na.name, arrivalUnixTime);
    }

    // area 이름 기반 예보 (ForecastProvider 가 캐시 키 area 로 직접 호출).
    ForecastResult getForecastByArea(const std::string& areaName, long arrivalUnixTime) {
        std::string response = fetchCityData(areaName);
        if (response.empty()) {
            Log::err("[Forecast] Empty response for " + areaName);
            return {1, 0, 0, 0, false};
        }
        try {
            auto j = nlohmann::json::parse(response);
            if (!j.contains("SeoulRtd.citydata_ppltn")) return {1, 0, 0, 0, false};
            auto& arr = j["SeoulRtd.citydata_ppltn"];
            if (!arr.is_array() || arr.empty())          return {1, 0, 0, 0, false};
            auto& data = arr[0];

            // 1) FCST_PPLTN 에서 arrivalTs 에 가장 가까운 예보 항목
            if (data.contains("FCST_PPLTN") && data["FCST_PPLTN"].is_array()
                && !data["FCST_PPLTN"].empty()) {
                const nlohmann::json* best = nullptr;
                long bestDelta = LONG_MAX; long bestEpoch = 0;
                for (auto& f : data["FCST_PPLTN"]) {
                    if (!f.contains("FCST_TIME") || !f["FCST_TIME"].is_string()) continue;
                    long ep = parseKstToEpoch(f["FCST_TIME"].get<std::string>());
                    if (ep == 0) continue;
                    long d = std::labs(ep - arrivalUnixTime);
                    if (d < bestDelta) { bestDelta = d; best = &f; bestEpoch = ep; }
                }
                if (best) {
                    int lvl = seoulLevelToInt(getStr(*best, "FCST_CONGEST_LVL"));
                    int mn  = getInt(*best, "FCST_PPLTN_MIN");
                    int mx  = getInt(*best, "FCST_PPLTN_MAX");
                    Log::info("[Forecast] " + areaName + " @arrival -> lvl " + std::to_string(lvl)
                              + " (Δ" + std::to_string(bestDelta/60) + "m)");
                    return {lvl, bestEpoch, mn, mx, true};
                }
            }

            // 2) 예보 부재 → 같은 payload 의 실시간 레벨로 대체 (fcstUnixTime=0 표식)
            if (data.contains("AREA_CONGEST_LVL") && data["AREA_CONGEST_LVL"].is_string()) {
                int lvl = seoulLevelToInt(data["AREA_CONGEST_LVL"].get<std::string>());
                Log::warn("[Forecast] " + areaName + " no FCST → realtime lvl " + std::to_string(lvl));
                return {lvl, 0, 0, 0, true};
            }
        } catch (const std::exception& e) {
            Log::err("[Forecast] Parse error for " + areaName + ": " + e.what());
        }
        return {1, 0, 0, 0, false};
    }

    // 최후 fallback 용: area 실시간 레벨만 (ForecastProvider 최종단계에서 사용).
    ExternalCongestionResult getRealtimeByArea(const std::string& areaName) {
        double lat, lng;
        if (!areaCoordinate(areaName, lat, lng)) return {1, "seoul", false};
        return getCongestion(lat, lng);
    }

private:
    static std::string getStr(const nlohmann::json& j, const char* k) {
        if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
        return "";
    }
    static int getInt(const nlohmann::json& j, const char* k) {
        if (!j.contains(k)) return 0;
        if (j[k].is_number_integer()) return j[k].get<int>();
        if (j[k].is_string()) { try { return std::stoi(j[k].get<std::string>()); } catch (...) {} }
        return 0;
    }

    // "YYYY-MM-DD HH:MM"(KST) → UTC epoch seconds. 파싱 실패 시 0.
    static long parseKstToEpoch(const std::string& s) {
        std::tm tm{};
        int y, mo, d, h, mi;
        if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
        tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = 0;
#if defined(_WIN32)
        long utc = static_cast<long>(_mkgmtime(&tm));
#else
        long utc = static_cast<long>(timegm(&tm));
#endif
        if (utc <= 0) return 0;
        return utc - 9 * 3600; // KST(UTC+9) → UTC
    }
};
