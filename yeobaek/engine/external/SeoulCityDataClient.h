#pragma once
#include "ExternalCongestionClient.h"
#include "../util/Logger.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <utility>
#include <mutex>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// [이식] 서울 실시간 도시데이터 API 클라이언트 (CrowdMap 자산)
//   문서: https://data.seoul.go.kr/dataList/OA-21285/F/1/datasetView.do
//   갱신주기: 5분 / 지역: 서울시 주요 121장소
//
// 여백에서의 역할:
//   - 실시간 AREA_CONGEST_LVL 조회 (모듈4 Plan B 의 최후 fallback)
//   - findNearestArea / 좌표 테이블: 장소↔예보지점 매핑(결정 C)과
//     SeoulCityDataForecastClient(예보) 의 공통 기반
//
// 원본 대비 변경점:
//   - 예보 클라이언트가 상속·재사용할 수 있도록 helper/테이블을 protected 로 노출
//   - build_area_map(결정 C) 용 공개 API nearestArea() 추가 (임계값 없이 최근접 + km)
// ─────────────────────────────────────────────────────────────────────────────
class SeoulCityDataClient : public ExternalCongestionClient {
public:
    explicit SeoulCityDataClient(std::string apiKey) : apiKey(std::move(apiKey)) {
        m_curl = curl_easy_init();
        if (m_curl) curl_easy_setopt(m_curl, CURLOPT_TCP_KEEPALIVE, 1L);
    }

    ~SeoulCityDataClient() override {
        if (m_curl) curl_easy_cleanup(m_curl);
    }

    bool covers(double lat, double lng) override {
        return lat >= 37.42 && lat <= 37.70 &&
               lng >= 126.76 && lng <= 127.18;
    }

    ExternalCongestionResult getCongestion(double lat, double lng) override {
        std::string areaName = findNearestArea(lat, lng);
        if (areaName.empty()) return {1, "seoul", false};

        std::string response = fetchCityData(areaName);
        if (response.empty()) {
            Log::err("[Seoul API] Empty response for " + areaName);
            return {1, "seoul", false};
        }

        try {
            auto j = nlohmann::json::parse(response);
            if (!j.contains("SeoulRtd.citydata_ppltn")) return {1, "seoul", false};
            auto& arr = j["SeoulRtd.citydata_ppltn"];
            if (!arr.is_array() || arr.empty())          return {1, "seoul", false};
            auto& data = arr[0];
            if (!data.is_object())                       return {1, "seoul", false};
            if (!data.contains("AREA_CONGEST_LVL"))      return {1, "seoul", false};
            if (!data["AREA_CONGEST_LVL"].is_string())   return {1, "seoul", false};

            std::string lvl = data["AREA_CONGEST_LVL"].get<std::string>();
            Log::info("[Seoul API] " + areaName + " -> " + lvl);
            return {seoulLevelToInt(lvl), "seoul", true};
        } catch (const std::exception& e) {
            Log::err("[Seoul API] Parse error for " + areaName + ": " + e.what());
            return {1, "seoul", false};
        }
    }

    // 121개 장소 좌표 테이블 (읽기 전용 노출) — PublicDataFeeder / 매핑에서 사용
    const std::vector<std::pair<std::string, std::pair<double, double>>>& getAreas() const {
        return seoulAreas;
    }

    // [신규 공개] build_area_map(결정 C) 용: 임계값 없이 최근접 예보지점 + 거리(km).
    //             found=false 이면 테이블이 비었다는 뜻(정상적으로는 항상 true).
    struct NearestArea { std::string name; double distKm; bool found; };
    NearestArea nearestArea(double lat, double lng) const {
        double best = 1e18; std::string name;
        for (auto& [n, c] : seoulAreas) {
            double d = haversineKm(lat, lng, c.first, c.second);
            if (d < best) { best = d; name = n; }
        }
        if (name.empty()) return {"", 0.0, false};
        return {name, best, true};
    }

    // 예보지점 이름 → 좌표 (예보 클라이언트가 area 이름만으로 조회할 때 사용)
    bool areaCoordinate(const std::string& name, double& lat, double& lng) const {
        for (auto& [n, c] : seoulAreas) {
            if (n == name) { lat = c.first; lng = c.second; return true; }
        }
        return false;
    }

protected:
    std::string apiKey;
    CURL*       m_curl = nullptr;
    std::mutex  m_curlMutex;

    // 원본과 동일: 임계값(약 30m²=0.0009 deg²) 안일 때만 매칭, 아니면 "" (실시간 스코프 보호)
    std::string findNearestArea(double lat, double lng) const {
        double minDist = 1e9; std::string nearest;
        for (auto& [name, coord] : seoulAreas) {
            double dLat = lat - coord.first;
            double dLng = lng - coord.second;
            double d = dLat * dLat + dLng * dLng;
            if (d < minDist) { minDist = d; nearest = name; }
        }
        return (minDist < 0.0009) ? nearest : "";
    }

    static int seoulLevelToInt(const std::string& lvl) {
        if (lvl == "여유")      return 1;
        if (lvl == "보통")      return 2;
        if (lvl == "약간 붐빔") return 3;
        if (lvl == "붐빔")      return 4;
        return 1;
    }

    // areaName 으로 citydata_ppltn 원본 JSON 을 가져온다 (실시간·예보 공통)
    std::string fetchCityData(const std::string& areaName) {
        // 키가 없으면 네트워크를 시도하지 않는다(오프라인/데모 안전 — 상위가 중립값 fallback).
        if (apiKey.empty()) return "";
        std::string url = "http://openapi.seoul.go.kr:8088/" + apiKey
                        + "/json/citydata_ppltn/1/1/" + urlEncode(areaName);
        return httpGet(url);
    }

    static double haversineKm(double lat1, double lng1, double lat2, double lng2) {
        constexpr double R = 6371.0088;
        auto rad = [](double d){ return d * M_PI / 180.0; };
        double dLat = rad(lat2 - lat1), dLng = rad(lng2 - lng1);
        double a = std::sin(dLat/2)*std::sin(dLat/2)
                 + std::cos(rad(lat1))*std::cos(rad(lat2))*std::sin(dLng/2)*std::sin(dLng/2);
        return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
    }

    std::string urlEncode(const std::string& s) {
        char* encoded = curl_easy_escape(nullptr, s.c_str(), static_cast<int>(s.length()));
        if (!encoded) return s;
        std::string result(encoded);
        curl_free(encoded);
        return result;
    }

    static size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string httpGet(const std::string& url) {
        std::lock_guard<std::mutex> lock(m_curlMutex);
        std::string response;
        if (m_curl) {
            curl_easy_reset(m_curl);
            curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 3L);
            curl_easy_setopt(m_curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_perform(m_curl);
        }
        return response;
    }

    // ── 121개 서울 주요장소 좌표 (CrowdMap 원본 유지) ──
    const std::vector<std::pair<std::string, std::pair<double,double>>> seoulAreas = {
        {"강남 MICE 관광특구", {37.5126, 127.0588}}, {"동대문 관광특구", {37.5717, 127.0093}},
        {"명동 관광특구", {37.5636, 126.9839}}, {"이태원 관광특구", {37.5347, 126.9947}},
        {"잠실 관광특구", {37.5133, 127.1000}}, {"종로·청계 관광특구", {37.5700, 126.9820}},
        {"홍대 관광특구", {37.5538, 126.9236}}, {"경복궁", {37.5796, 126.9770}},
        {"광화문·덕수궁", {37.5759, 126.9769}}, {"보신각", {37.5697, 126.9836}},
        {"서울 암사동 유적", {37.5512, 127.1267}}, {"창덕궁·종묘", {37.5825, 126.9914}},
        {"가산디지털단지역", {37.4817, 126.8826}}, {"강남역", {37.4979, 127.0276}},
        {"건대입구역", {37.5402, 127.0695}}, {"고덕역", {37.5550, 127.1543}},
        {"고속터미널역", {37.5050, 127.0049}}, {"교대역", {37.4934, 127.0143}},
        {"구로디지털단지역", {37.4854, 126.9015}}, {"구로역", {37.5031, 126.8819}},
        {"군자역", {37.5571, 127.0796}}, {"대림역", {37.4929, 126.8956}},
        {"동대문역", {37.5712, 127.0094}}, {"뚝섬역", {37.5471, 127.0473}},
        {"미아사거리역", {37.6131, 127.0301}}, {"발산역", {37.5587, 126.8377}},
        {"사당역", {37.4766, 126.9816}}, {"삼각지역", {37.5347, 126.9733}},
        {"서울대입구역", {37.4813, 126.9527}}, {"서울식물원·마곡나루역", {37.5670, 126.8294}},
        {"서울역", {37.5547, 126.9706}}, {"선릉역", {37.5045, 127.0489}},
        {"성신여대입구역", {37.5926, 127.0163}}, {"수유역", {37.6378, 127.0254}},
        {"신논현역·논현역", {37.5048, 127.0252}}, {"신도림역", {37.5089, 126.8911}},
        {"신림역", {37.4842, 126.9298}}, {"신촌·이대역", {37.5559, 126.9368}},
        {"양재역", {37.4847, 127.0344}}, {"역삼역", {37.5006, 127.0366}},
        {"연신내역", {37.6190, 126.9213}}, {"오목교역·목동운동장", {37.5247, 126.8757}},
        {"왕십리역", {37.5613, 127.0379}}, {"용산역", {37.5298, 126.9648}},
        {"이태원역", {37.5346, 126.9946}}, {"장지역", {37.4784, 127.1262}},
        {"장한평역", {37.5614, 127.0644}}, {"천호역", {37.5384, 127.1238}},
        {"총신대입구(이수)역", {37.4865, 126.9818}}, {"충정로역", {37.5601, 126.9636}},
        {"합정역", {37.5497, 126.9136}}, {"혜화역", {37.5826, 127.0019}},
        {"홍대입구역(2호선)", {37.5572, 126.9244}}, {"회기역", {37.5897, 127.0570}},
        {"쌍문역", {37.6483, 127.0345}}, {"신정네거리역", {37.5197, 126.8567}},
        {"잠실새내역", {37.5117, 127.0865}}, {"잠실역", {37.5133, 127.1000}},
        {"시의회 앞", {37.5662, 126.9779}}, {"숭례문", {37.5599, 126.9753}},
        {"가락시장", {37.4926, 127.1182}}, {"가로수길", {37.5202, 127.0228}},
        {"광장(전통)시장", {37.5703, 127.0001}}, {"김포공항", {37.5586, 126.7944}},
        {"노량진", {37.5125, 126.9420}}, {"덕수궁길·정동길", {37.5663, 126.9743}},
        {"북촌한옥마을", {37.5826, 126.9831}}, {"서촌", {37.5797, 126.9700}},
        {"성수카페거리", {37.5446, 127.0559}}, {"압구정로데오거리", {37.5273, 127.0397}},
        {"여의도", {37.5219, 126.9243}}, {"연남동", {37.5611, 126.9234}},
        {"영등포 타임스퀘어", {37.5172, 126.9034}}, {"용리단길", {37.5304, 126.9706}},
        {"이태원 앤틱가구거리", {37.5347, 126.9933}}, {"인사동", {37.5717, 126.9853}},
        {"창동 신경제 중심지", {37.6534, 127.0473}}, {"청담동 명품거리", {37.5251, 127.0473}},
        {"청량리 제기동 일대 전통시장", {37.5803, 127.0285}}, {"해방촌·경리단길", {37.5430, 126.9852}},
        {"DDP(동대문디자인플라자)", {37.5670, 127.0094}}, {"DMC(디지털미디어시티)", {37.5798, 126.8896}},
        {"북창동 먹자골목", {37.5634, 126.9787}}, {"남대문시장", {37.5599, 126.9779}},
        {"익선동", {37.5733, 126.9876}}, {"잠실롯데타워·석촌호수", {37.5126, 127.1025}},
        {"송리단길·호수단길", {37.5089, 127.1075}}, {"신촌 스타광장", {37.5559, 126.9385}},
        {"강서한강공원", {37.5878, 126.8334}}, {"고척돔", {37.4982, 126.8671}},
        {"광나루한강공원", {37.5447, 127.1228}}, {"광화문광장", {37.5728, 126.9769}},
        {"국립중앙박물관·용산가족공원", {37.5240, 126.9803}}, {"난지한강공원", {37.5683, 126.8769}},
        {"남산공원", {37.5512, 126.9882}}, {"노들섬", {37.5174, 126.9588}},
        {"뚝섬한강공원", {37.5301, 127.0670}}, {"망원한강공원", {37.5530, 126.8967}},
        {"반포한강공원", {37.5111, 126.9956}}, {"북서울꿈의숲", {37.6219, 127.0420}},
        {"서리풀공원·몽마르뜨공원", {37.4953, 127.0054}}, {"서울대공원", {37.4275, 127.0162}},
        {"서울숲공원", {37.5444, 127.0376}}, {"아차산", {37.5563, 127.1029}},
        {"양화한강공원", {37.5388, 126.8979}}, {"어린이대공원", {37.5489, 127.0817}},
        {"여의도한강공원", {37.5283, 126.9325}}, {"월드컵공원", {37.5709, 126.8784}},
        {"응봉산", {37.5478, 127.0276}}, {"이촌한강공원", {37.5179, 126.9707}},
        {"잠실종합운동장", {37.5159, 127.0731}}, {"잠실한강공원", {37.5191, 127.0824}},
        {"잠원한강공원", {37.5237, 127.0102}}, {"청계산", {37.4329, 127.0526}},
        {"보라매공원", {37.4933, 126.9197}}, {"서대문독립공원", {37.5723, 126.9590}},
        {"안양천", {37.5235, 126.8703}}, {"여의서로", {37.5286, 126.9197}},
        {"올림픽공원", {37.5210, 127.1217}}, {"홍제폭포", {37.5942, 126.9445}},
        {"송현녹지광장", {37.5765, 126.9810}},
    };
};
