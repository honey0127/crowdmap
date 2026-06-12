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
#   [추가] EpollServer.cpp, PublicDataFeeder.cpp
#   [제외] UserCountManager.cpp / SlidingWindow.cpp / DeadSessionSweeper.cpp
#          → epoll + SpatialDensityEngine 아키텍처로 대체되어 더 이상 빌드에 포함하지 않음.
#            (Ghost 유저 정리는 GridZone 5분 만료 + SpatialDensityEngine::globalCleanup 가 담당)
set(SOURCES
        main.cpp
        ThreadPool.cpp
        ZoneMapper.cpp
        CongestionCalculator.cpp
        EpollServer.cpp
        PublicDataFeeder.cpp
)

# 실행 파일 생성
add_executable(crowdmap_server ${SOURCES})

# libcurl 찾기
find_package(CURL REQUIRED)

# nlohmann/json (Seoul/Daegu 클라이언트 JSON 파싱용)
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

# hiredis (Redis C 클라이언트)
find_library(HIREDIS_LIB hiredis REQUIRED)
find_path(HIREDIS_INCLUDE hiredis/hiredis.h REQUIRED)

# epoll 기반 서버는 Linux 전용이다.
#   (EpollServer 가 <sys/epoll.h> 를 사용 → Windows 빌드는 지원하지 않음)
if(WIN32)
    message(FATAL_ERROR
            "이 서버는 epoll 기반이라 Linux 에서만 빌드/실행됩니다. (Windows 미지원)")
else()
    target_link_libraries(crowdmap_server
            pthread CURL::libcurl nlohmann_json::nlohmann_json ${HIREDIS_LIB})
endif()

target_include_directories(crowdmap_server PRIVATE ${HIREDIS_INCLUDE})
