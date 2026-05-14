#pragma once
#include <iostream>
#include <mutex>
#include <string>

namespace Log {
    inline std::mutex mtx;

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
