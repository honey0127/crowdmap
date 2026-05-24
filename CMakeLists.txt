cmake_minimum_required(VERSION 3.14)
project(CrowdMapServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 소스 파일 목록
set(SOURCES
    server/main.cpp
    server/ThreadPool.cpp
    server/ZoneMapper.cpp
    server/UserCountManager.cpp
    server/CongestionCalculator.cpp
    server/SlidingWindow.cpp
    server/DeadSessionSweeper.cpp
)

# 실행 파일 생성
add_executable(crowdmap_server ${SOURCES})

# libcurl 찾기
find_package(CURL REQUIRED)

# nlohmann/json (SeoulCityDataClient JSON 파싱용)
# 시스템에 설치된 경우(sudo apt install nlohmann-json3-dev) 그것을 사용,
# 없으면 FetchContent로 자동 다운로드
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Windows에서 Winsock 라이브러리 링크
if(WIN32)
    target_link_libraries(crowdmap_server ws2_32 CURL::libcurl nlohmann_json::nlohmann_json)
else()
    target_link_libraries(crowdmap_server pthread CURL::libcurl nlohmann_json::nlohmann_json)
endif()