cmake_minimum_required(VERSION 3.14)
project(CrowdMapServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 성능 최적화 컴파일 옵션
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O3 -march=native -flto -ffast-math -pthread)
    add_link_options(-flto -pthread)
endif()

# 소스 파일 목록
set(SOURCES
    main.cpp
    EpollServer.cpp
    ThreadPool.cpp
    ZoneMapper.cpp
    CongestionCalculator.cpp
)

# 실행 파일 생성
add_executable(crowdmap_server ${SOURCES})

# libcurl 찾기
find_package(CURL REQUIRED)

# nlohmann/json
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

# 라이브러리 링크
if(WIN32)
    target_link_libraries(crowdmap_server ws2_32 CURL::libcurl nlohmann_json::nlohmann_json)
else()
    target_link_libraries(crowdmap_server pthread CURL::libcurl nlohmann_json::nlohmann_json)
endif()