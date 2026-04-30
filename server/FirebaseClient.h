#ifndef FIREBASECLIENT_H
#define FIREBASECLIENT_H

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Place {
    std::string id;
    std::string name;
    double latitude;
    double longitude;
    int capacity;
};

struct Zone {
    int zoneId;
    double baseLat;
    double baseLng;
    double gridSize;
};

class FirebaseClient {
private:
    std::string projectId;
    std::string baseUrl;
    
    // curl 응답 콜백
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s);
    
    // JSON 파싱 헬퍼
    json parseFirestoreResponse(const std::string& response);

public:
    FirebaseClient(const std::string& projectId);
    
    // 장소 정보 로드
    std::vector<Place> getPlaces();
    
    // Zone 정보 로드
    std::vector<Zone> getZones();
    
    // 혼잡도 이력 저장
    bool saveCongestionHistory(int zoneId, int userCount, 
                              const std::string& level, double ratio);
};

#endif // FIREBASECLIENT_H