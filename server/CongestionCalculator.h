#ifndef CONGESTIONCALCULATOR_H
#define CONGESTIONCALCULATOR_H

#include <string>

enum class CongestionLevel {
    RELAXED,    // 여유
    MODERATE,   // 보통
    CROWDED     // 혼잡
};

struct CongestionResult {
    CongestionLevel level;
    double ratio;      // 혼잡도 비율 (0.0 ~ 1.0+)
    int userCount;
    std::string levelString() const;
};

class CongestionCalculator {
private:
    // 장소별 수용 인원 (임시로 고정값 사용, 나중에 DB에서 로드)
    static constexpr int DEFAULT_CAPACITY = 100;

public:
    CongestionResult calculateCongestion(int userCount, int capacity = DEFAULT_CAPACITY);
};

#endif // CONGESTIONCALCULATOR_H