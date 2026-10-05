#pragma once
#include <iostream>
#include <mutex>
#include <string>
#include <cstdlib>

namespace Log {
    inline std::mutex mtx;

    // 줄마다 비운다(std::endl): 표준출력이 파일·파이프(start.sh 의 tee, 실험 스크립트)로
    // 가면 완전 버퍼링이라, 비우지 않으면 4KB 가 찰 때까지 로그가 보이지 않는다.
    // 핫패스 로그는 아래처럼 debug 로만 남기므로 비우는 비용은 문제가 되지 않는다.
    //
    // 핫패스(연결/메시지 단위) 로그는 debug 레벨로 강등한다.
    // 전역 뮤텍스 + stdout 로깅은 이벤트 루프를 터미널 I/O 속도로 직렬화하므로,
    // 운영 시에는 주기 통계(Stats) 한 줄만 남기고 개별 이벤트 로그는
    // CROWDMAP_DEBUG=1 환경변수를 줬을 때만 출력한다.
    inline const bool debugEnabled = [] {
        const char* v = std::getenv("CROWDMAP_DEBUG");
        return v != nullptr && *v != '\0' && std::string(v) != "0";
    }();

    inline void debug(const std::string& msg) {
        if (!debugEnabled) return;
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "[DEBUG] " << msg << std::endl;
    }
    inline void info(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "[INFO]  " << msg << std::endl;
    }
    inline void warn(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "[WARN]  " << msg << std::endl;
    }
    inline void err(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cerr << "[ERROR] " << msg << "\n";
    }
}
