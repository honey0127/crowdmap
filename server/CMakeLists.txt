cmake_minimum_required(VERSION 3.10)
project(CrowdMapServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 소스 파일 목록
set(SOURCES
    main.cpp
    ThreadPool.cpp
    ZoneMapper.cpp
    UserCountManager.cpp
    CongestionCalculator.cpp
    SlidingWindow.cpp
    DeadSessionSweeper.cpp
)

# 실행 파일 생성
add_executable(crowdmap_server ${SOURCES})

# libcurl 찾기
find_package(CURL REQUIRED)

# Windows에서 Winsock 라이브러리 링크
if(WIN32)
    target_link_libraries(crowdmap_server ws2_32 CURL::libcurl)
else()
    target_link_libraries(crowdmap_server pthread CURL::libcurl)
endif()