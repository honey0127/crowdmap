cmake_minimum_required(VERSION 3.15)
project(yeobaek_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O3 -pthread)
endif()

# ── Python ──
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

# ── pybind11: pip 설치본(권장) → 없으면 GitHub FetchContent (절차서 0-5) ──
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
        OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(PYBIND11_CMAKE_DIR)
        find_package(pybind11 CONFIG QUIET PATHS "${PYBIND11_CMAKE_DIR}")
    endif()
endif()
if(NOT pybind11_FOUND)
    include(FetchContent)
    FetchContent_Declare(pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(pybind11)
endif()

# ── libcurl (서울 도시데이터 HTTP) ──
find_package(CURL REQUIRED)

# ── nlohmann/json (FCST_PPLTN 파싱) ──
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ── 파이썬 확장 모듈 (헤더-온리 엔진 + 바인딩 1 TU) ──
pybind11_add_module(yeobaek_engine bindings/pybind_module.cpp)
target_link_libraries(yeobaek_engine PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json)
target_compile_definitions(yeobaek_engine PRIVATE YEOBAEK_ENGINE_VERSION="0.1.0")

# 빌드 산출물(.so)을 server/ 옆에 두어 FastAPI 가 바로 import 하도록 복사
add_custom_command(TARGET yeobaek_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:yeobaek_engine>
            ${CMAKE_CURRENT_SOURCE_DIR}/../server/$<TARGET_FILE_NAME:yeobaek_engine>
    COMMENT "Copy yeobaek_engine module into server/")
