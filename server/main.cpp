#include <iostream>
#include <thread>
#include <string>
#include <sstream>
#include <memory>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include "CacheManager.h"
#include "SeoulCityDataClient.h"
#include "CongestionRouter.h"
#include "SlidingWindow.h"
#include "DeadSessionSweeper.h"

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
    #define SOCKET_ERROR -1
    #define closesocket close
    typedef int SOCKET;
#endif

#include "ThreadPool.h"
#include "ZoneMapper.h"
#include "UserCountManager.h"
#include "CongestionCalculator.h"
#include "FirebaseClient.h"

// 글로벌 객체들
std::unique_ptr<ThreadPool> threadPool;
ZoneMapper zoneMapper;
UserCountManager userCountManager;
CongestionCalculator congestionCalculator;
std::unique_ptr<FirebaseClient> firebaseClient;
CacheManager cacheManager(30);
std::unique_ptr<CongestionRouter> congestionRouter;
const std::string SEOUL_API_KEY = "sample";
SlidingWindow slidingWindow(300);  // 5분 윈도우 (300초)

const int PORT = 5001;
const int THREAD_POOL_SIZE = 4;
const std::string FIREBASE_PROJECT_ID = "crowdmap-50936";

// 클라이언트 요청 처리 함수
void handleClient(SOCKET clientSocket, int clientId) {
    char buffer[256] = {0};

    try {
        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

            if (bytesReceived <= 0) {
                std::cout << "Client " << clientId << " disconnected\n";
                break;
            }

            buffer[bytesReceived] = '\0';

            std::istringstream iss(buffer);
            int userId;
            double latitude, longitude;
            char comma;

            if (!(iss >> userId >> comma >> latitude >> comma >> longitude)) {
                std::cerr << "[Client " << clientId << "] Invalid data format\n";
                continue;
            }

            std::cout << "[Client " << clientId << "] Received: userId=" << userId
                      << ", lat=" << latitude << ", lng=" << longitude << "\n";

            // 1. Zone 변환
            int zoneId = zoneMapper.coordinateToZoneId(latitude, longitude);
            std::cout << "[Client " << clientId << "] Zone ID: " << zoneId << "\n";

            // 2. 사용자 수 업데이트 (userId가 0인 경우는 조회 전용이므로 업데이트 제외)
            if (userId != 0) {
                slidingWindow.addEvent(userId, zoneId);
                userCountManager.updateUserLocation(userId, zoneId);
            }
            int zoneCount = userCountManager.getZoneCount(zoneId);

            // 3. 혼잡도 계산 (Router 활용: 캐시 -> 외부 API -> 내부 계산 fallback)
            CongestionResult result = congestionRouter->resolve(latitude, longitude, zoneCount, cacheManager, zoneId);


            // 4. 응답 전송
            std::string response = std::string(result.levelString()) + "|"
                                 + std::to_string(result.ratio);
            if (send(clientSocket, response.c_str(), response.length(), 0) == SOCKET_ERROR) {
                std::cerr << "[Client " << clientId << "] Send failed\n";
                break;
            }
            std::cout << "[Client " << clientId << "] Response sent: " << response << "\n\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception in handleClient: " << e.what() << "\n";
    }

    closesocket(clientSocket);
}

// 메인 서버 루프
void runServer() {
    SOCKET serverSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Socket creation failed\n";
        return;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed\n";
        closesocket(serverSocket);
        return;
    }

    if (listen(serverSocket, 5) == SOCKET_ERROR) {
        std::cerr << "Listen failed\n";
        closesocket(serverSocket);
        return;
    }

    std::cout << "Server started on port " << PORT << "\n";
    std::cout << "Waiting for connections...\n\n";

    int clientIdCounter = 1;
    while (true) {
        SOCKET clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "Accept failed\n";
            continue;
        }
        int currentClientId = clientIdCounter++;
        std::cout << "Client " << currentClientId << " connected from "
                  << inet_ntoa(clientAddr.sin_addr) << "\n";
        threadPool->enqueue([clientSocket, currentClientId]() {
            handleClient(clientSocket, currentClientId);
        });
    }

    closesocket(serverSocket);
}

int main() {
    std::cout << "=== CrowdMap Server with Firebase ===\n\n";

    threadPool = std::make_unique<ThreadPool>(THREAD_POOL_SIZE);
    std::cout << "ThreadPool initialized with " << THREAD_POOL_SIZE << " workers\n\n";

    firebaseClient = std::make_unique<FirebaseClient>(FIREBASE_PROJECT_ID);
    auto places = firebaseClient->getPlaces();
    std::cout << "Loaded " << places.size() << " places\n";
    auto zones = firebaseClient->getZones();
    std::cout << "Loaded " << zones.size() << " zones\n\n";

    // CongestionRouter 초기화 및 클라이언트 등록
    congestionRouter = std::make_unique<CongestionRouter>();
    congestionRouter->addClient(std::make_shared<SeoulCityDataClient>(SEOUL_API_KEY));
    std::cout << "CongestionRouter initialized with SeoulCityDataClient\n";

    DeadSessionSweeper deadSweeper(userCountManager, slidingWindow, 300);
    deadSweeper.start();

    // 서버 시작
    runServer();

    return 0;
}