#include <iostream>
#include <thread>
#include <string>
#include <sstream>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <mutex>
#include <curl/curl.h>
#include "Logger.h"
#include "CacheManager.h"
#include "SlidingWindow.h"
#include "DeadSessionSweeper.h"
#include "CongestionRouter.h"

#ifdef _WIN32
#include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR   -1
#define closesocket    close
typedef int SOCKET;
#endif

#include "ThreadPool.h"
#include "ZoneMapper.h"
#include "UserCountManager.h"
#include "CongestionCalculator.h"
#include "SeoulCityDataClient.h"
#include "DaeguTrafficClient.h"   // ← 추가

static std::string requireEnv(const char* key) {
    const char* val = std::getenv(key);
    if (!val) {
        Log::err(std::string("필수 환경변수 없음: ") + key
                 + " — start.sh 또는 .env 확인");
        std::exit(1);
    }
    return val;
}

static std::string optionalEnv(const char* key, const std::string& defaultVal) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : defaultVal;
}

static std::unique_ptr<ThreadPool>        threadPool;
static std::unique_ptr<CongestionRouter>  router;

static ZoneMapper       zoneMapper;
static UserCountManager userCountManager;
static CacheManager     cacheManager(30, 1000);
static SlidingWindow    slidingWindow(300);

static std::mutex                                            zoneMutexMapLock;
static std::unordered_map<int, std::shared_ptr<std::mutex>> zoneMutexMap;

static std::shared_ptr<std::mutex> getZoneMutex(int zoneId) {
    std::lock_guard<std::mutex> lock(zoneMutexMapLock);
    auto& ptr = zoneMutexMap[zoneId];
    if (!ptr) ptr = std::make_shared<std::mutex>();
    return ptr;
}

static std::string recvLine(SOCKET sock) {
    std::string line;
    char c;
    while (true) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return "";
        if (c == '\n') break;
        if (c != '\r') line += c;
    }
    return line;
}

static void handleClient(SOCKET clientSocket, int clientId) {
    const std::string cid = "[Client " + std::to_string(clientId) + "] ";

    try {
        while (true) {
            std::string line = recvLine(clientSocket);
            if (line.empty()) {
                Log::info(cid + "disconnected");
                break;
            }

            std::istringstream iss(line);
            int    userId;
            double latitude, longitude;
            char   comma;

            if (!(iss >> userId >> comma >> latitude >> comma >> longitude)) {
                Log::warn(cid + "invalid format → \"" + line + "\"");
                continue;
            }

            Log::info(cid + "userId=" + std::to_string(userId)
                      + " lat=" + std::to_string(latitude)
                      + " lng=" + std::to_string(longitude));

            int zoneId = zoneMapper.coordinateToZoneId(latitude, longitude);
            if (zoneId == -1) {
                Log::warn(cid + "out-of-ZoneMapper-range coord → RELAXED");
                const std::string resp = "RELAXED|0.0\n";
                send(clientSocket, resp.c_str(), static_cast<int>(resp.length()), 0);
                continue;
            }

            if (userId != 0) {
                slidingWindow.addEvent(userId, zoneId);
                userCountManager.updateUserLocation(userId, zoneId);
            }
            int zoneCount = userCountManager.getZoneCount(zoneId);

            CongestionResult result;

            if (cacheManager.get(zoneId, result)) {
                Log::info(cid + "cache HIT zone=" + std::to_string(zoneId));
            } else {
                auto zoneMtx = getZoneMutex(zoneId);
                std::unique_lock<std::mutex> fetchLock(*zoneMtx);

                if (!cacheManager.get(zoneId, result)) {
                    Log::info(cid + "cache MISS, fetching zone="
                              + std::to_string(zoneId));
                    result = router->resolve(
                            latitude, longitude, zoneCount, cacheManager, zoneId);
                }
            }

            std::string response = std::string(result.levelString()) + "|"
                                   + std::to_string(result.ratio) + "\n";

            if (send(clientSocket,
                     response.c_str(),
                     static_cast<int>(response.length()), 0) == SOCKET_ERROR) {
                Log::err(cid + "send failed");
                break;
            }
            Log::info(cid + "→ " + response);
        }

    } catch (const std::exception& e) {
        Log::err(cid + "exception: " + e.what());
    }

    closesocket(clientSocket);
}

static void runServer(int port) {
    SOCKET serverSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        Log::err("socket creation failed");
        return;
    }

    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&opt), sizeof(opt));

    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port        = htons(static_cast<uint16_t>(port));

    if (bind(serverSocket,
             reinterpret_cast<struct sockaddr*>(&serverAddr),
             sizeof(serverAddr)) == SOCKET_ERROR) {
        Log::err("bind failed on port " + std::to_string(port));
        closesocket(serverSocket);
        return;
    }

    if (listen(serverSocket, 10) == SOCKET_ERROR) {
        Log::err("listen failed");
        closesocket(serverSocket);
        return;
    }

    Log::info("server listening on port " + std::to_string(port));

    int clientIdCounter = 1;
    while (true) {
        SOCKET clientSocket = accept(
                serverSocket,
                reinterpret_cast<struct sockaddr*>(&clientAddr),
                &clientAddrLen);

        if (clientSocket == INVALID_SOCKET) {
            Log::err("accept failed");
            continue;
        }

        int cid = clientIdCounter++;
        Log::info("client " + std::to_string(cid)
                  + " connected from " + inet_ntoa(clientAddr.sin_addr));

        threadPool->enqueue([clientSocket, cid]() {
            handleClient(clientSocket, cid);
        });
    }

    closesocket(serverSocket);
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);

    std::cout << "=== CrowdMap Server ===\n\n";

    // 1. 환경변수에서 비밀값 로드
    const std::string seoulApiKey = requireEnv("SEOUL_API_KEY");
    const std::string daeguApiKey = "e9e9d88d3877d92fe087f11a1588490687cc4a5fd87a82b02bdf9c9c6fca5638";  // ← 대구 API 키 입력
    const int port = std::stoi(optionalEnv("SERVER_PORT", "8765"));

    // 2. ThreadPool 동적 크기
    const int workerCount = std::max(
            static_cast<int>(std::thread::hardware_concurrency()) * 2, 4);
    threadPool = std::make_unique<ThreadPool>(workerCount);
    Log::info("ThreadPool: " + std::to_string(workerCount) + " workers"
              + " (cores=" + std::to_string(std::thread::hardware_concurrency()) + ")");

    // 3. CongestionRouter 구성
    //    우선순위: SeoulCityDataClient → DaeguTrafficClient → 내부 계산
    router = std::make_unique<CongestionRouter>();
    router->addClient(std::make_shared<SeoulCityDataClient>(seoulApiKey));
    router->addClient(std::make_shared<DaeguTrafficClient>(daeguApiKey));  // ← 추가
    Log::info("CongestionRouter ready (Seoul + Daegu registered)");

    // 4. 고스트 유저 정리 백그라운드 스레드 시작
    DeadSessionSweeper deadSweeper(userCountManager, slidingWindow, 300);
    deadSweeper.start();

    // 5. 서버 시작
    runServer(port);

    return 0;
}