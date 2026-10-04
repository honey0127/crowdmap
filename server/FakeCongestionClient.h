#pragma once
#include "ExternalCongestionClient.h"
#include <chrono>
#include <mutex>
#include <thread>

// 실험 D 용 가짜 외부 API: 고정 지연 후 '보통'을 돌려준다.
// 실제 클라이언트(SeoulCityDataClient)가 CURL 핸들 1개를 mutex 로 직렬화하므로
// 여기서도 한 번에 1건만 호출되게 해 같은 조건을 만든다.
class FakeCongestionClient : public ExternalCongestionClient {
public:
    explicit FakeCongestionClient(int latencyMs) : latencyMs_(latencyMs) {}

    bool covers(double, double) override { return true; }

    ExternalCongestionResult getCongestion(double, double) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::this_thread::sleep_for(std::chrono::milliseconds(latencyMs_));
        return {2, "fake", true};
    }

private:
    int        latencyMs_;
    std::mutex mutex_;
};
