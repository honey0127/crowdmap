#include "CongestionCalculator.h"

CongestionResult CongestionCalculator::calculateCongestion(int userCount, int capacity) {
    double ratio = static_cast<double>(userCount) / capacity;
    
    CongestionLevel level;
    if (ratio < 0.4) {
        level = CongestionLevel::RELAXED;
    } else if (ratio < 0.7) {
        level = CongestionLevel::MODERATE;
    } else {
        level = CongestionLevel::CROWDED;
    }
    
    return {level, ratio, userCount};
}

std::string CongestionResult::levelString() const {
    switch (level) {
        case CongestionLevel::RELAXED:
            return "RELAXED";
        case CongestionLevel::MODERATE:
            return "MODERATE";
        case CongestionLevel::CROWDED:
            return "CROWDED";
        default:
            return "UNKNOWN";
    }
}