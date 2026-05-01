#pragma once
#include <string>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

struct PlaceCongestion {
    std::string poiId;
    std::string poiName;
    int congestionLevel;  // 1:여유 2:보통 3:혼잡 4:매우혼잡
    double congestion;
    std::string datetime;
};

class SKTClient {
public:
    SKTClient(const std::string& appKey) : appKey(appKey) {}

    // 특정 장소 혼잡도 조회
    PlaceCongestion getCongestion(const std::string& poiId) {
        std::string url = BASE_URL + poiId;
        std::string response = httpGet(url);

        PlaceCongestion result;
        try {
            auto json = nlohmann::json::parse(response);
            result.poiId = json["contents"]["poiId"];
            result.poiName = json["contents"]["poiName"];
            if (!json["contents"]["rltm"].empty()) {
                auto rltm = json["contents"]["rltm"][0];
                result.congestionLevel = rltm["congestionLevel"];
                result.congestion = rltm["congestion"];
                result.datetime = rltm["datetime"];
            }
        } catch (...) {
            result.congestionLevel = 1;
        }
        return result;
    }

private:
    std::string appKey;
    const std::string BASE_URL = 
        "https://apis.openapi.sk.com/puzzle/place/congestion/rltm/pois/";

    static size_t writeCallback(void* contents, size_t size, 
                                size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string httpGet(const std::string& url) {
        CURL* curl = curl_easy_init();
        std::string response;
        if (curl) {
            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, 
                ("appkey: " + appKey).c_str());
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_perform(curl);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
        return response;
    }
};