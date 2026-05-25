#pragma once
#include "ExternalCongestionClient.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <utility>
#include <iostream>

/*
 * @file DaeguTrafficClient.h
 * @brief 대구광역시 실시간 교통소통정보 API 클라이언트
 *
 * 동작 방식:
 *   1. 사용자 좌표와 가장 가까운 대구 도로 구간 찾기 (거리 제한 없음)
 *      → 대구 어디를 탭해도 항상 가장 가까운 구간 속도로 혼잡도 반환
 *   2. 해당 구간의 실시간 속도 API 호출
 *   3. 속도 → 혼잡도 레벨(1~3) 변환
 *
 * 혼잡도 변환 기준:
 *   속도 30km/h 이상 → 1 (여유)
 *   속도 15~30km/h  → 2 (보통)
 *   속도 15km/h 미만 → 3 (혼잡)
 *   */

class DaeguTrafficClient : public ExternalCongestionClient {
public:
    DaeguTrafficClient(const std::string& apiKey) : apiKey(apiKey) {}

    // 대구 지역 좌표인지 확인
    bool covers(double lat, double lng) override {
        return lat >= 35.65 && lat <= 36.00 &&
               lng >= 128.40 && lng <= 128.80;
    }

    // 대구 실시간 교통 혼잡도 조회
    ExternalCongestionResult getCongestion(double lat, double lng) override {
        // 1. 가장 가까운 구간 ID 찾기 (거리 제한 없이 항상 반환)
        std::string linkId = findNearestLink(lat, lng);

        // 2. API 호출
        std::string url = "https://apis.data.go.kr/6270000/service/rest1/linkspeed"
                  "?serviceKey=" + apiKey +
                  "&pageNo=1&numOfRows=1000&type=json";
        std::string response = httpGet(url);

        // 3. 해당 구간 속도 찾아서 레벨 반환
        try {
            auto j = nlohmann::json::parse(response);
            auto& items = j["body"]["items"]["item"];

            for (auto& item : items) {
                if (item["STD_LINK_ID"].get<std::string>() == linkId) {
                    int speed = item["LINK_SPEED"].get<int>();
                    int level = speedToLevel(speed);
                    std::cout << "[Daegu API] "
                              << item["SECTION_NM"].get<std::string>()
                              << " 속도: " << speed << "km/h"
                              << " 레벨: " << level << "\n";
                    return {level, "daegu", true};
                }
            }
            return {1, "daegu", false};

        } catch (const std::exception& e) {
            std::cerr << "[Daegu API] 오류: " << e.what() << "\n";
            return {1, "daegu", false};
        }
    }

private:
    std::string apiKey;

    // 대구 주요 도로 구간 좌표 매핑
    // 사람이 많은 주요 장소 위주로 등록
    // 매핑 안 된 장소는 가장 가까운 구간으로 자동 연결됨
    const std::vector<std::pair<std::string, std::pair<double, double>>> daeguLinks = {
            // 달구벌대로
            {"1500000100", {35.8561, 128.6270}},  // 수성네거리
            {"1500000200", {35.8561, 128.6280}},  // 수성교남단
            {"1500000503", {35.8714, 128.5940}},  // 반고개-신남
            {"1500000603", {35.8720, 128.5930}},  // 신남-반고개

            // 동성로/중앙로
            {"1500001000", {35.8704, 128.5955}},  // 중앙네거리
            {"1500001100", {35.8688, 128.5966}},  // 동성로

            // 경북대 주변
            {"1500002000", {35.8863, 128.6095}},  // 경북대 북문
            {"1500002100", {35.8830, 128.6070}},  // 경북대 정문

            // 동대구역 주변
            {"1500003000", {35.8796, 128.6281}},  // 동대구역 앞
            {"1500003100", {35.8780, 128.6300}},  // 동대구역 주변

            // 반월당
            {"1500004000", {35.8660, 128.5950}},  // 반월당네거리
            {"1500004100", {35.8655, 128.5960}},  // 반월당 일대

            // 수성구
            {"1500005000", {35.8575, 128.6305}},  // 수성못
            {"1500005100", {35.8540, 128.6350}},  // 수성구청

            // 북구
            {"1500006000", {35.9000, 128.5890}},  // 칠성시장
            {"1500006100", {35.8960, 128.5870}},  // 북구 일대

            // 달서구
            {"1500007000", {35.8430, 128.5330}},  // 성서 일대
            {"1500007100", {35.8500, 128.5500}},  // 달서구청 주변
    };

    // 가장 가까운 구간 ID 반환 (거리 제한 없이 항상 반환)
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
        return nearest;  // 항상 가장 가까운 구간 반환
    }

    // 속도 → 혼잡도 레벨
    int speedToLevel(int speed) {
        if (speed >= 30) return 1;  // 여유
        if (speed >= 15) return 2;  // 보통
        return 3;                   // 혼잡
    }

    static size_t writeCallback(void* contents, size_t size,
                                size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
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
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }
        return response;
    }
};