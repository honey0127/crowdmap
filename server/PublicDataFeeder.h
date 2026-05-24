#ifndef PUBLIC_DATA_FEEDER_H
#define PUBLIC_DATA_FEEDER_H

#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include "SpatialDensityEngine.h"
#include "SeoulCityDataClient.h"

/**
 * @brief 공공 데이터 피더 (PublicDataFeeder)
 * 서울시 주요 121개 장소의 실시간 혼잡도 데이터를 주기적으로 가져와
 * SpatialDensityEngine에 가상 사용자 데이터를 주입합니다.
 */
class PublicDataFeeder {
public:
    PublicDataFeeder(SpatialDensityEngine& engine, const std::string& apiKey);
    ~PublicDataFeeder();

    void start(); // 백그라운드 스레드 시작
    void stop();  // 백그라운드 스레드 중지

private:
    void loop(); // 5분 주기 실행 루프

    // 혼잡도 레벨을 가상 인구수로 변환
    int levelToVirtualUserCount(int level);

    SpatialDensityEngine& m_engine;
    std::unique_ptr<SeoulCityDataClient> m_apiClient;

    std::thread m_thread;
    std::atomic<bool> m_running;

    static constexpr int FETCH_INTERVAL_SEC = 300; // 5분
};

#endif // PUBLIC_DATA_FEEDER_H
