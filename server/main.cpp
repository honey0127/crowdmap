#include <iostream>
#include <thread>
#include <string>
#include <sstream>
#include <memory>
#include <cstring>

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
#include "CacheManager.h"
#include "CongestionRouter.h"
#include "SeoulCityDataClient.h"
#include "FirebaseClient.h"

// ── 서버 설정 ────────────────────────────────────────────
const int         PORT             = 5001;
const int         THREAD_POOL_SIZE = 4;
const std::string FIREBASE_PROJECT = "crowdmap-50936";
const std::string SEOUL_API_KEY    = "5159637a53646c7834304d4343766c";

// ── 글로벌 객체 ──────────────────────────────────────────
std::unique_ptr<ThreadPool>     threadPool;
std::unique_ptr<FirebaseClient> firebaseClient;

ZoneMapper        zoneMapper;
UserCountManager  userCountManager;
CacheManager      cacheManager(300);   // TTL 300초
CongestionRouter  congestionRouter;

// ── 클라이언트 요청 처리 ─────────────────────────────────
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

            // 파싱: "userId,latitude,longitude"
            std::istringstream iss(buffer);
            int    userId;
            double latitude, longitude;
            char   comma;

            if (!(iss >> userId >> comma >> latitude >> comma >> longitude)) {
                std::cerr << "[Client " << clientId << "] Invalid format\n";
                continue;
            }

            std::cout << "[Client " << clientId << "] userId=" << userId
                      << " lat=" << latitude << " lng=" << longitude << "\n";

            // 1. 좌표 → zone
            int zoneId = zoneMapper.coordinateToZoneId(latitude, longitude);

            // 2. 사용자 수 갱신
            userCountManager.updateUserLocation(userId, zoneId);
            int zoneCount = userCountManager.getZoneCount(zoneId);

            // 3. 혼잡도 결정 (외부 API → 캐시 → 내부 계산 순)
            CongestionResult result = congestionRouter.resolve(
                latitude, longitude, zoneCount, cacheManager, zoneId
            );

            std::cout << "[Client " << clientId << "] Zone=" << zoneId
                      << " count=" << zoneCount
                      << " level=" << result.levelString() << "\n";

            // 4. 응답 전송: "RELAXED|0.25"
            std::string response = result.levelString() + "|"
                                 + std::to_string(result.ratio);

            if (send(clientSocket, response.c_str(), response.length(), 0) == SOCKET_ERROR) {
                std::cerr << "[Client " << clientId << "] Send failed\n";
                break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception in handleClient: " << e.what() << "\n";
    }

    closesocket(clientSocket);
}

// ── 서버 루프 ────────────────────────────────────────────
void runServer() {
    SOCKET serverSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return;
    }
#endif

    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Socket creation failed\n";
        return;
    }

    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port        = htons(PORT);

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

    std::cout << "Server started on port " << PORT << "\n"
              << "Waiting for connections...\n\n";

    int clientIdCounter = 1;
    while (true) {
        SOCKET clientSocket = accept(serverSocket,
                                     (struct sockaddr*)&clientAddr,
                                     &clientAddrLen);
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "Accept failed\n";
            continue;
        }

        int opt = 1;
        setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        int cid = clientIdCounter++;
        std::cout << "Client " << cid << " connected from "
                  << inet_ntoa(clientAddr.sin_addr) << "\n";

        threadPool->enqueue([clientSocket, cid]() {
            handleClient(clientSocket, cid);
        });
    }

    closesocket(serverSocket);
}

// ── main ─────────────────────────────────────────────────
int main() {
    std::cout << "=== CrowdMap Server ===\n\n";

    // ThreadPool
    threadPool = std::make_unique<ThreadPool>(THREAD_POOL_SIZE);
    std::cout << "ThreadPool: " << THREAD_POOL_SIZE << " workers\n";

    // Firebase에서 장소/zone 데이터 로드
    firebaseClient = std::make_unique<FirebaseClient>(FIREBASE_PROJECT);
    auto places = firebaseClient->getPlaces();
    auto zones  = firebaseClient->getZones();
    std::cout << "Loaded " << places.size() << " places, "
              << zones.size() << " zones\n\n";

    // 혼잡도 API 라우터 구성
    // 우선순위 1: 서울 실시간 도시데이터 (서울 bbox 커버)
    congestionRouter.addClient(
        std::make_shared<SeoulCityDataClient>(SEOUL_API_KEY)
    );
    // 우선순위 2: 추후 다른 지자체 API 추가 시 여기에 addClient()
    // congestionRouter.addClient(std::make_shared<DaeguCityDataClient>(...));

    std::cout << "CongestionRouter ready\n\n";

    // 서버 시작
    runServer();

    return 0;
}