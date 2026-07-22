#pragma once
#include "SpatialHash.h"
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cmath>
#include <mutex>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// [신규] SpatialIndex — 반경 후보 필터 (모듈1, 결정 E)
//
// SpatialHash 그리드 셀에 (content_id, lat, lng) 포인트를 버킷팅해두고,
// query_radius(lat,lng,km) 시 반경을 덮는 셀만 순회 → haversine 로 정밀 컷.
// e5 코사인 유사도(Python numpy)로 넘길 "작은 후보집합" 을 만드는 게 목적.
//
// 서버 부팅 시 places 를 add() 로 적재(절차서 1-4). read-heavy 이므로 단순 뮤텍스.
// ─────────────────────────────────────────────────────────────────────────────
class SpatialIndex {
public:
    void add(int64_t contentId, double lat, double lng) {
        std::lock_guard<std::mutex> lock(mtx);
        int64_t key = SpatialHash::generateKey(lat, lng);
        cells[key].push_back({contentId, lat, lng});
        ++count;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        cells.clear(); count = 0;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return count;
    }

    // 반경(km) 내 content_id 목록. 자기 자신(같은 좌표)도 포함될 수 있으니 호출측에서 제외.
    std::vector<int64_t> queryRadius(double lat, double lng, double radiusKm) const {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<int64_t> out;

        // 위도 1도 ≈ 111km. 경도는 위도에 따라 축소.
        double dLatDeg = radiusKm / 111.0;
        double cosLat = std::cos(lat * M_PI / 180.0);
        double dLngDeg = radiusKm / (111.0 * std::max(0.01, cosLat));

        int64_t latLo = SpatialHash::latIndex(lat - dLatDeg);
        int64_t latHi = SpatialHash::latIndex(lat + dLatDeg);
        int64_t lngLo = SpatialHash::lonIndex(lng - dLngDeg);
        int64_t lngHi = SpatialHash::lonIndex(lng + dLngDeg);

        for (int64_t la = latLo; la <= latHi; ++la) {
            for (int64_t lo = lngLo; lo <= lngHi; ++lo) {
                auto it = cells.find(SpatialHash::cellKey(la, lo));
                if (it == cells.end()) continue;
                for (const auto& p : it->second) {
                    if (haversineKm(lat, lng, p.lat, p.lng) <= radiusKm)
                        out.push_back(p.id);
                }
            }
        }
        return out;
    }

    static double haversineKm(double lat1, double lng1, double lat2, double lng2) {
        constexpr double R = 6371.0088;
        auto rad = [](double d){ return d * M_PI / 180.0; };
        double dLat = rad(lat2 - lat1), dLng = rad(lng2 - lng1);
        double a = std::sin(dLat/2)*std::sin(dLat/2)
                 + std::cos(rad(lat1))*std::cos(rad(lat2))*std::sin(dLng/2)*std::sin(dLng/2);
        return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
    }

private:
    struct Pt { int64_t id; double lat; double lng; };
    std::unordered_map<int64_t, std::vector<Pt>> cells;
    size_t count = 0;
    mutable std::mutex mtx;
};
