#pragma once
#include "ExternalCongestionClient.h"
#include "Logger.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <utility>

/*
 * @file DaeguTrafficClient.h
 * @brief 대구광역시 실시간 교통소통정보 API 클라이언트
 *
 * 동작 방식:
 *   1. 사용자 좌표와 가장 가까운 대구 도로 구간 찾기
 *   2. 해당 구간의 실시간 속도 API 호출
 *   3. 속도 → 혼잡도 레벨(1~3) 변환
 *
 * 혼잡도 변환 기준:
 *   속도 30km/h 이상 → 1 (여유)
 *   속도 15~30km/h  → 2 (보통)
 *   속도 15km/h 미만 → 3 (혼잡)
 */

class DaeguTrafficClient : public ExternalCongestionClient {
public:
    DaeguTrafficClient(const std::string& apiKey) : apiKey(apiKey) {
        if (apiKey.empty()) {
            Log::warn("[DaeguTrafficClient] DAEGU_API_KEY 없음 — 이 클라이언트는 비활성화됩니다");
        }
    }

    // API 키가 없으면 covers()에서 false → router가 자동으로 건너뜀
    bool covers(double lat, double lng) override {
        if (apiKey.empty()) return false;
        return lat >= 35.65 && lat <= 36.00 &&
               lng >= 128.40 && lng <= 128.80;
    }

    ExternalCongestionResult getCongestion(double lat, double lng) override {
        if (apiKey.empty()) return {1, "daegu", false};

        std::string linkId = findNearestLink(lat, lng);
        if (linkId.empty()) {
            Log::warn("[Daegu API] 가장 가까운 구간을 찾지 못했습니다");
            return {1, "daegu", false};
        }

        std::string url =
            "https://apis.data.go.kr/6270000/service/rest1/linkspeed"
            "?serviceKey=" + apiKey +
            "&pageNo=1&numOfRows=1000&type=json";

        std::string response = httpGet(url);

        // ── 응답이 비었는지 확인 ──────────────────────────────────────
        if (response.empty()) {
            Log::err("[Daegu API] 빈 응답 (네트워크 오류 또는 타임아웃)");
            return {1, "daegu", false};
        }

        // ── JSON 시작 문자 확인 (HTML/XML 오류 페이지 걸러내기) ────────
        // 공공데이터포털은 인증 실패 시 XML or HTML을 반환하기도 함
        char firstChar = response[0];
        if (firstChar != '{' && firstChar != '[') {
            // 앞 200자만 로깅 (HTML 전체를 출력하지 않도록)
            std::string preview = response.substr(0, std::min((int)response.size(), 200));
            Log::err("[Daegu API] JSON이 아닌 응답 수신 (API 키 오류 또는 엔드포인트 문제)");
            Log::err("[Daegu API] 응답 미리보기: " + preview);
            return {1, "daegu", false};
        }

        // ── JSON 파싱 ─────────────────────────────────────────────────
        try {
            auto j = nlohmann::json::parse(response);

            // 공공데이터포털 표준 오류 응답 확인
            if (j.contains("resultCode") || j.contains("errMsg")) {
                std::string code = j.value("resultCode", "");
                std::string msg  = j.value("errMsg", "unknown");
                Log::err("[Daegu API] API 오류 응답: code=" + code + " msg=" + msg);
                return {1, "daegu", false};
            }

            // 정상 데이터 탐색
            if (!j.contains("body") || !j["body"].contains("items")) {
                Log::err("[Daegu API] 응답에 body.items 없음");
                return {1, "daegu", false};
            }

            auto& items = j["body"]["items"]["item"];
            if (!items.is_array()) {
                Log::err("[Daegu API] items.item 이 배열이 아님");
                return {1, "daegu", false};
            }

            for (auto& item : items) {
                std::string itemLinkId = item.value("STD_LINK_ID", "");
                if (itemLinkId == linkId) {
                    int speed = item.value("LINK_SPEED", 0);
                    int level = speedToLevel(speed);
                    std::string sectionName = item.value("SECTION_NM", linkId);
                    Log::info("[Daegu API] " + sectionName
                              + " 속도: " + std::to_string(speed) + "km/h"
                              + " 레벨: " + std::to_string(level));
                    return {level, "daegu", true};
                }
            }

            Log::warn("[Daegu API] linkId=" + linkId + " 를 응답에서 찾지 못함");
            return {1, "daegu", false};

        } catch (const nlohmann::json::parse_error& e) {
            Log::err("[Daegu API] JSON 파싱 실패: " + std::string(e.what()));
            // 파싱 실패 시 응답 앞부분 출력 (디버깅용)
            Log::err("[Daegu API] 응답 미리보기: "
                     + response.substr(0, std::min((int)response.size(), 200)));
            return {1, "daegu", false};
        } catch (const std::exception& e) {
            Log::err("[Daegu API] 예외: " + std::string(e.what()));
            return {1, "daegu", false};
        }
    }

private:
    std::string apiKey;

    const std::vector<std::pair<std::string, std::pair<double, double>>> daeguLinks = {
        // 달구벌대로
        {"1500000100", {35.8561, 128.6270}},
        {"1500000200", {35.8561, 128.6280}},
        {"1500000503", {35.8714, 128.5940}},
        {"1500000603", {35.8720, 128.5930}},
        // 동성로/중앙로
        {"1500001000", {35.8704, 128.5955}},
        {"1500001100", {35.8688, 128.5966}},
        // 경북대 주변
        {"1500002000", {35.8863, 128.6095}},
        {"1500002100", {35.8830, 128.6070}},
        // 동대구역 주변
        {"1500003000", {35.8796, 128.6281}},
        {"1500003100", {35.8780, 128.6300}},
        // 반월당
        {"1500004000", {35.8660, 128.5950}},
        {"1500004100", {35.8655, 128.5960}},
        // 수성구
        {"1500005000", {35.8575, 128.6305}},
        {"1500005100", {35.8540, 128.6350}},
        // 북구
        {"1500006000", {35.9000, 128.5890}},
        {"1500006100", {35.8960, 128.5870}},
        // 달서구
        {"1500007000", {35.8430, 128.5330}},
        {"1500007100", {35.8500, 128.5500}},
    };

    std::string findNearestLink(double lat, double lng) {
        double minDist = 1e9;
        std::string nearest;
        for (auto& [id, coord] : daeguLinks) {
            double d = (lat - coord.first) * (lat - coord.first)
                     + (lng - coord.second) * (lng - coord.second);
            if (d < minDist) {
                minDist = d;
                nearest = id;
            }
        }
        return nearest;
    }

    int speedToLevel(int speed) {
        if (speed >= 30) return 1;
        if (speed >= 15) return 2;
        return 3;
    }

    static size_t writeCallback(void* contents, size_t size,
                                size_t nmemb, std::string* userp) {
        userp->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    std::string httpGet(const std::string& url) {
        CURL* curl = curl_easy_init();
        std::string response;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // 리다이렉트 허용
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }
        return response;
    }
};