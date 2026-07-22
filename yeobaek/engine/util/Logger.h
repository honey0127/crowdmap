#pragma once
#include <iostream>
#include <mutex>
#include <string>
#include <cstdlib>

// [이식] CrowdMap Logger. 개별 이벤트 로그는 YEOBAEK_DEBUG=1 일 때만 출력한다.
namespace Log {
    inline std::mutex mtx;

    inline const bool debugEnabled = [] {
        const char* v = std::getenv("YEOBAEK_DEBUG");
        return v != nullptr && *v != '\0' && std::string(v) != "0";
    }();

    inline void debug(const std::string& msg) {
        if (!debugEnabled) return;
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "[DEBUG] " << msg << "\n";
    }
    inline void info(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "[INFO]  " << msg << "\n";
    }
    inline void warn(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "[WARN]  " << msg << "\n";
    }
    inline void err(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cerr << "[ERROR] " << msg << "\n";
    }
}
