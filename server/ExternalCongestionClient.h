#pragma once
#include <string>

struct ExternalCongestionResult {
    int level;          // 1:여유 2:보통 3:혼잡 4:매우혼잡
    std::string source; // "seoul", "public", "internal"
    bool valid;
};

class ExternalCongestionClient {
public:
    virtual ~ExternalCongestionClient() = default;
    virtual ExternalCongestionResult getCongestion(double lat, double lng) = 0;
    virtual bool covers(double lat, double lng) = 0; // 이 API가 해당 좌표를 지원하는지
};