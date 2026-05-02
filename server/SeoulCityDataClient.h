#pragma once
#include "ExternalCongestionClient.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <map>
#include <string>

// 서울 실시간 도시데이터 API
// 문서: https://data.seoul.go.kr/dataList/OA-21285/S/1/datasetView.do
// 인증: 공공데이터포털 API 키 (무료)
// 지원 구역: 종로, 명동, 홍대, 강남 등 115개 핫스팟
class SeoulCityDataClient : public ExternalCongestionClient {
public:
    SeoulCityDataClient(const std::string& apiKey) : apiKey(apiKey) {}

   bool covers(double lat, double lng) override {
    return true;  // 일단 전국 다 허용
}

    ExternalCongestionResult getCongestion(double lat, double lng) override {
        std::string areaName = findNearestArea(lat, lng);
        if (areaName.empty()) return {1, "seoul", false};

        std::string url = "http://openapi.seoul.go.kr:8088/" + apiKey
                        + "/json/citydata_ppltn/1/1/" + urlEncode(areaName);
        std::string response = httpGet(url);

        try {
            auto j = nlohmann::json::parse(response);
            auto& data = j["SeoulRtd.citydata_ppltn"][0];
            std::string lvl = data["AREA_CONGEST_LVL"].get<std::string>();
            return {seoulLevelToInt(lvl), "seoul", true};
        } catch (...) {
            return {1, "seoul", false};
        }
    }

private:
    std::string apiKey;

    // 서울 주요 지역 좌표 → 지역명 매핑
    // 실제 서비스에서는 Firebase zones에 저장
    const std::vector<std::pair<std::string, std::pair<double,double>>> seoulAreas = {
        {"종로·청계",  {37.5700, 126.9820}},
        {"명동·남대문", {37.5636, 126.9839}},
        {"홍대·합정",  {37.5538, 126.9236}},
        {"강남·역삼",  {37.4979, 127.0276}},
        {"잠실·송파",  {37.5133, 127.1000}},
        {"신촌·이대",  {37.5546, 126.9368}},
        {"건대입구",   {37.5402, 127.0695}},
        {"인사동·익선", {37.5744, 126.9862}},
    };

    std::string findNearestArea(double lat, double lng) {
        double minDist = 1e9;
        std::string nearest;
        for (auto& [name, coord] : seoulAreas) {
            double d = (lat - coord.first)*(lat - coord.first)
                     + (lng - coord.second)*(lng - coord.second);
            if (d < minDist) { minDist = d; nearest = name; }
        }
        // 2km 이상이면 해당 없음
        return (minDist < 0.0004) ? nearest : "";
    }

    int seoulLevelToInt(const std::string& lvl) {
        if (lvl == "여유")       return 1;
        if (lvl == "보통")       return 2;
        if (lvl == "약간 붐빔")  return 3;
        if (lvl == "붐빔")      return 4;
        return 1;
    }

    std::string urlEncode(const std::string& s) {
        // 한국어 URL 인코딩 (curl 사용)
        CURL* curl = curl_easy_init();
        char* encoded = curl_easy_escape(curl, s.c_str(), s.length());
        std::string result(encoded);
        curl_free(encoded);
        curl_easy_cleanup(curl);
        return result;
    }

    static size_t writeCallback(void* c, size_t s, size_t n, std::string* u) {
        u->append((char*)c, s*n); return s*n;
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