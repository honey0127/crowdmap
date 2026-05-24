#include "EpollServer.h"
#include "Logger.h"
#include <iostream>
#include <cstring>
#include <charconv>

EpollServer::EpollServer(uint16_t port, 
                         SpatialDensityEngine& engine,
                         ZoneMapper& zm, 
                         CongestionRouter& cr,
                         CacheManager& cm)
    : pool_(4096), densityEngine_(engine), 
      zoneMapper_(zm), congestionRouter_(cr), cacheManager_(cm) {
    
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
// ... (rest of constructor) ...
}

// ... (handleAccept, handleRead, etc.) ...

void EpollServer::parseLine(ClientContext& ctx, std::string_view line) {
    if (line.empty()) return;

    size_t comma1 = line.find(',');
    size_t comma2 = line.find(',', comma1 + 1);
    if (comma1 == std::string_view::npos || comma2 == std::string_view::npos) return;

    std::string_view user_id_sv = line.substr(0, comma1);
    std::string_view lat_sv = line.substr(comma1 + 1, comma2 - comma1 - 1);
    std::string_view lon_sv = line.substr(comma2 + 1);

    int userId = 0;
    double lat = 0.0, lon = 0.0;
    
    std::from_chars(user_id_sv.data(), user_id_sv.data() + user_id_sv.size(), userId);
    lat = std::atof(std::string(lat_sv).c_str());
    lon = std::atof(std::string(lon_sv).c_str());

    // ── Spatial-Concurrency Data Layer 통합 ──
    if (userId != 0) {
        // [Update] 위치 갱신: 데이터만 기록하고 응답을 보내지 않음 (Silent Update)
        densityEngine_.recordLocation(userId, lat, lon);
        return;
    }

    // [Query] 조회 요청 (userId == 0): 맵 클릭 조회 시에만 응답 수행
    size_t localDensity = densityEngine_.getDensity(lat, lon);

    // 구역 단위 혼잡도 처리를 위한 기존 로직 유지 (Seoul API 등 연동)
    int zoneId = zoneMapper_.coordinateToZoneId(lat, lon);
    std::string response;

    if (zoneId == -1) {
        response = "RELAXED|0.0\n";
    } else {
        CongestionResult result;
        if (!cacheManager_.get(zoneId, result)) {
            // localDensity를 기반으로 혼잡도 계산
            result = congestionRouter_.resolve(lat, lon, static_cast<int>(localDensity), cacheManager_, zoneId);
        }
        response = std::string(result.levelString()) + "|" + std::to_string(result.ratio) + "\n";
    }

    // 오직 조회 요청에 대해서만 응답을 전송하여 클라이언트의 수신 버퍼 오염 방지
    send(ctx.fd, response.c_str(), response.length(), 0);
}

