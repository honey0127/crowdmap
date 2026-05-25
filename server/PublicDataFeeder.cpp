#include "PublicDataFeeder.h"
#include "Logger.h"
#include <chrono>

PublicDataFeeder::PublicDataFeeder(SpatialDensityEngine& engine, const std::string& apiKey)
    : m_engine(engine),
      m_apiClient(std::make_unique<SeoulCityDataClient>(apiKey)),
      m_running(false) {}

PublicDataFeeder::~PublicDataFeeder() {
    stop();
}

void PublicDataFeeder::start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&PublicDataFeeder::loop, this);
    Log::info("[PublicDataFeeder] Background thread started (Interval: 300s)");
}

void PublicDataFeeder::stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void PublicDataFeeder::loop() {
    while (m_running) {
        Log::info("[PublicDataFeeder] Starting sync with Seoul City Data API...");

        const auto& areas = m_apiClient->getAreas();
        int virtualUserCounter = 900000; // 가상 유저 ID 시작점

        for (const auto& area : areas) {
            if (!m_running) break;

            const std::string& areaName = area.first;
            double lat = area.second.first;
            double lon = area.second.second;

            // [Guard Clause] 유효 좌표 범위 검증 (대한민국 위경도 범위)
            if (lat < 33.0 || lat > 39.0 || lon < 124.0 || lon > 132.0) {
                Log::warn("[PublicDataFeeder] Skipping area " + areaName + " due to invalid coords: " +
                          std::to_string(lat) + ", " + std::to_string(lon));
                continue;
            }

            // 1. API 호출 (HTTP GET)
            ExternalCongestionResult res = m_apiClient->getCongestion(lat, lon);

            if (res.valid) {
                // 2. 혼잡도 레벨에 따른 가상 유저 수 결정
                int userCount = levelToVirtualUserCount(res.level);

                // 3. SpatialDensityEngine에 가상 유저 주입
                // 동일 좌표 그리드(SpatialKey)에 여러 유저 이벤트를 발생시켜 밀도 형성
                for (int i = 0; i < userCount; ++i) {
                    m_engine.recordLocation(virtualUserCounter++, lat, lon);
                }

                Log::info("[PublicDataFeeder] Injected " + std::to_string(userCount) +
                          " virtual users for " + areaName);
            }

            // API 과부하 방지를 위해 지역 간 미세 지연 (100ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        Log::info("[PublicDataFeeder] Sync cycle completed. Waiting 5 minutes...");

        // [중요] 5분 대기 (중간에 stop 신호 감지 위해 1초씩 나눠서 대기)
        for (int i = 0; i < FETCH_INTERVAL_SEC && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

int PublicDataFeeder::levelToVirtualUserCount(int level) {
    switch (level) {
        case 4: return 100; // 붐빔
        case 3: return 50;  // 약간 붐빔
        case 2: return 20;  // 보통
        case 1: return 5;   // 여유
        default: return 0;
    }
}
