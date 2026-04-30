#include "FirebaseClient.h"
#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

size_t FirebaseClient::WriteCallback(void* contents, size_t size, 
                                     size_t nmemb, std::string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

FirebaseClient::FirebaseClient(const std::string& projectId) {
    this->projectId = projectId;
    baseUrl = "https://firestore.googleapis.com/v1/projects/" 
              + projectId + "/databases/(default)/documents";
}

json FirebaseClient::parseFirestoreResponse(const std::string& response) {
    try {
        return json::parse(response);
    } catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << "\n";
        return json({});
    }
}

std::vector<Place> FirebaseClient::getPlaces() {
    std::vector<Place> places;
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (!curl) {
        std::cerr << "CURL init failed\n";
        return places;
    }
    
    std::string url = baseUrl + "/places";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        std::cerr << "CURL request failed: " << curl_easy_strerror(res) << "\n";
        curl_easy_cleanup(curl);
        return places;
    }
    
    // JSON 파싱
    try {
        json data = parseFirestoreResponse(response);
        
        if (data.contains("documents")) {
            for (const auto& doc : data["documents"]) {
                Place place;
                
                // 문서 ID 추출
                std::string fullPath = doc["name"].get<std::string>();
                size_t lastSlash = fullPath.find_last_of("/");
                place.id = fullPath.substr(lastSlash + 1);
                
                // 필드 추출
                if (doc.contains("fields")) {
                    auto fields = doc["fields"];
                    
                    if (fields.contains("name") && fields["name"].contains("stringValue")) {
                        place.name = fields["name"]["stringValue"].get<std::string>();
                    }
                    if (fields.contains("latitude") && fields["latitude"].contains("doubleValue")) {
                        auto latValue = fields["latitude"]["doubleValue"];
                        place.latitude = latValue.is_string() ? std::stod(latValue.get<std::string>()) : latValue.get<double>();
                    }
                    if (fields.contains("longitude") && fields["longitude"].contains("doubleValue")) {
                        auto lngValue = fields["longitude"]["doubleValue"];
                        place.longitude = lngValue.is_string() ? std::stod(lngValue.get<std::string>()) : lngValue.get<double>();
                    }
                    if (fields.contains("capacity") && fields["capacity"].contains("integerValue")) {
                        place.capacity = std::stoi(fields["capacity"]["integerValue"].get<std::string>());
                    }
                    
                    places.push_back(place);
                    std::cout << "[Firebase] Loaded place: " << place.name << " (cap: " 
                              << place.capacity << ")\n";
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing places: " << e.what() << "\n";
    }
    
    curl_easy_cleanup(curl);
    return places;
}

std::vector<Zone> FirebaseClient::getZones() {
    std::vector<Zone> zones;
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (!curl) {
        std::cerr << "CURL init failed\n";
        return zones;
    }
    
    std::string url = baseUrl + "/zones";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        std::cerr << "CURL request failed: " << curl_easy_strerror(res) << "\n";
        curl_easy_cleanup(curl);
        return zones;
    }
    
    try {
        json data = parseFirestoreResponse(response);
        
        if (data.contains("documents")) {
            for (const auto& doc : data["documents"]) {
                Zone zone;
                
                if (doc.contains("fields")) {
                    auto fields = doc["fields"];
                    
                    if (fields.contains("zoneId") && fields["zoneId"].contains("integerValue")) {
                        zone.zoneId = std::stoi(fields["zoneId"]["integerValue"].get<std::string>());
                    }
                    if (fields.contains("baseLat") && fields["baseLat"].contains("doubleValue")) {
                        auto latValue = fields["baseLat"]["doubleValue"];
                        zone.baseLat = latValue.is_string() ? std::stod(latValue.get<std::string>()) : latValue.get<double>();
                    }
                    if (fields.contains("baseLng") && fields["baseLng"].contains("doubleValue")) {
                        auto lngValue = fields["baseLng"]["doubleValue"];
                        zone.baseLng = lngValue.is_string() ? std::stod(lngValue.get<std::string>()) : lngValue.get<double>();
                    }
                    if (fields.contains("gridSize") && fields["gridSize"].contains("doubleValue")) {
                        auto sizeValue = fields["gridSize"]["doubleValue"];
                        zone.gridSize = sizeValue.is_string() ? std::stod(sizeValue.get<std::string>()) : sizeValue.get<double>();
                    }
                    
                    zones.push_back(zone);
                    std::cout << "[Firebase] Loaded zone: " << zone.zoneId << "\n";
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing zones: " << e.what() << "\n";
    }
    
    curl_easy_cleanup(curl);
    return zones;
}

bool FirebaseClient::saveCongestionHistory(int zoneId, int userCount,
                                           const std::string& level, double ratio) {
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (!curl) {
        std::cerr << "CURL init failed\n";
        return false;
    }
    
    // 현재 타임스탄프
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::ostringstream timestamp;
    timestamp << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%SZ");
    
    // JSON 바디 구성
    std::ostringstream body;
    body << "{"
         << "\"fields\": {"
         << "  \"zoneId\":    {\"integerValue\": \"" << zoneId << "\"},"
         << "  \"userCount\": {\"integerValue\": \"" << userCount << "\"},"
         << "  \"level\":     {\"stringValue\": \"" << level << "\"},"
         << "  \"ratio\":     {\"doubleValue\": " << ratio << "},"
         << "  \"timestamp\": {\"timestampValue\": \"" << timestamp.str() << "\"}"
         << "}"
         << "}";
    
    std::string url = baseUrl + "/congestion_history";
    std::string bodyStr = body.str();
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        std::cerr << "[Firebase] Save failed: " << curl_easy_strerror(res) << "\n";
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return false;
    }
    
    std::cout << "[Firebase] Saved history - zone:" << zoneId 
              << " count:" << userCount << " level:" << level << "\n";
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return true;
}