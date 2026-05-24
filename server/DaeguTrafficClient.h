#pragma once
#include "ExternalCongestionClient.h"
#include "Logger.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <utility>
#include <mutex>

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
    // ── [FIX 1] apiKey를 저장해두고 URL 생성 시 실제로 사용 ──────────
    explicit DaeguTrafficClient(const std::string& key) : apiKey(key) {
        m_curl = curl_easy_init();
        if (m_curl) {
            curl_easy_setopt(m_curl, CURLOPT_TCP_KEEPALIVE, 1L);
        }
    }

    ~DaeguTrafficClient() {
        if (m_curl) curl_easy_cleanup(m_curl);
    }

    bool covers(double lat, double lng) override {
        return lat >= 35.65 && lat <= 36.00 &&
               lng >= 128.40 && lng <= 128.80;
    }

    ExternalCongestionResult getCongestion(double lat, double lng) override {
        std::string linkId = findNearestLink(lat, lng);
        if (linkId.empty()) {
            Log::warn("[Daegu API] No nearest link found for lat="
                      + std::to_string(lat) + " lng=" + std::to_string(lng));
            return {1, "daegu", false};
        }

        // ── [FIX 2] apiKey 멤버 변수를 URL에 실제로 사용 ─────────────
        //            numOfRows를 1000으로 늘려 전체 구간 포함
        std::string url = "https://apis.data.go.kr/6270000/service/rest1"
                          "?serviceKey=" + apiKey +
                          "&pageNo=1&numOfRows=1000&type=json";

        std::string response = httpGet(url);

        if (response.empty()) {
            Log::err("[Daegu API] Empty response (linkId=" + linkId + ")");
            return {1, "daegu", false};
        }

        try {
            auto j = nlohmann::json::parse(response);

            // ── [FIX 3] 응답 구조 존재 여부 단계별 확인 ─────────────
            if (!j.contains("body")) {
                Log::warn("[Daegu API] No 'body' in response");
                return {1, "daegu", false};
            }
            if (!j["body"].contains("items") ||
                !j["body"]["items"].contains("item")) {
                Log::warn("[Daegu API] No items in response");
                return {1, "daegu", false};
            }

            auto& items = j["body"]["items"]["item"];
            if (!items.is_array()) {
                Log::warn("[Daegu API] 'item' is not an array");
                return {1, "daegu", false};
            }

            for (auto& item : items) {
                // ── [FIX 4] STD_LINK_ID: 문자열·숫자 모두 안전하게 비교 ──
                std::string itemLinkId;
                if (item.contains("STD_LINK_ID")) {
                    if (item["STD_LINK_ID"].is_string())
                        itemLinkId = item["STD_LINK_ID"].get<std::string>();
                    else
                        itemLinkId = std::to_string(
                                item["STD_LINK_ID"].get<long long>());
                }

                if (itemLinkId != linkId) continue;

                // ── [FIX 5] LINK_SPEED: 문자열·숫자 모두 안전하게 파싱 ──
                int speed = 0;
                if (item.contains("LINK_SPEED")) {
                    if (item["LINK_SPEED"].is_number())
                        speed = item["LINK_SPEED"].get<int>();
                    else if (item["LINK_SPEED"].is_string()) {
                        try { speed = std::stoi(
                                    item["LINK_SPEED"].get<std::string>()); }
                        catch (...) { speed = 0; }
                    }
                }

                int level = speedToLevel(speed);
                std::string sectionName =
                        (item.contains("SECTION_NM") &&
                         item["SECTION_NM"].is_string())
                        ? item["SECTION_NM"].get<std::string>()
                        : linkId;

                Log::info("[Daegu API] " + sectionName
                          + " 속도=" + std::to_string(speed) + "km/h"
                          + " 레벨=" + std::to_string(level));
                return {level, "daegu", true};
            }

            // linkId가 응답에 없는 경우 — 로그 남기고 폴백
            Log::warn("[Daegu API] linkId=" + linkId + " not found in response ("
                      + std::to_string(items.size()) + " items returned)");
            return {1, "daegu", false};

        } catch (const std::exception& e) {
            Log::err("[Daegu API] Parse error: " + std::string(e.what())
                     + " | raw=" + response.substr(0, 200));
            return {1, "daegu", false};
        }
    }

private:
    std::string apiKey;   // ← 실제로 URL에 쓰임
    CURL*       m_curl = nullptr;
    std::mutex  m_curlMutex;

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
            double d = (lat - coord.first)  * (lat - coord.first)
                       + (lng - coord.second) * (lng - coord.second);
            if (d < minDist) { minDist = d; nearest = id; }
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

    // ── [FIX 6] SeoulCityDataClient처럼 CURL 핸들 재사용 ─────────────
    std::string httpGet(const std::string& url) {
        std::lock_guard<std::mutex> lock(m_curlMutex);
        std::string response;
        if (m_curl) {
            curl_easy_reset(m_curl);
            curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(m_curl, CURLOPT_TCP_KEEPALIVE, 1L);
            CURLcode res = curl_easy_perform(m_curl);
            if (res != CURLE_OK) {
                Log::err("[Daegu API] curl error: "
                         + std::string(curl_easy_strerror(res)));
            }
        }
        return response;
    }
};