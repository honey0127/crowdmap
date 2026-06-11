#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "MemoryPool.h"
#include "SpatialDensityEngine.h"
#include "ZoneMapper.h"
#include "CongestionRouter.h"
#include "CacheManager.h"
#include "ThreadPool.h"

/**
 * @brief 연결별 컨텍스트
 *
 * inbuf 는 TCP 패킷 분할(packet splitting)을 흡수하기 위한 누적 버퍼다.
 * recv 로 들어온 바이트를 여기에 모았다가 '\n' 단위로 잘라서 한 줄씩 처리한다.
 * 덕분에 1바이트씩 recv 하던 기존 recvLine 방식의 잦은 시스템 콜이 사라진다.
 *
 * lastActivity 는 유휴 연결 정리에 쓴다. 셀룰러 NAT 뒤의 단말은 FIN 없이
 * 사라지는 경우가 많아, 이 시각 기준으로 죽은 연결을 회수하지 않으면
 * fd 가 무한히 누적된다.
 */
struct ClientContext {
    int         fd         = -1;
    uint64_t    generation = 0;
    std::string inbuf;
    std::chrono::steady_clock::time_point lastActivity;
};

/**
 * @brief epoll 기반 단일 리액터(reactor) 서버
 *
 *  - accept / recv 등 I/O 다중화는 epoll 단일 스레드가 담당한다.
 *  - 위치 갱신(userId != 0)은 SpatialDensityEngine 에 기록만 하고 응답하지 않는다(Silent Update).
 *  - 조회(userId == 0)는 외부 API 호출 등 블로킹이 발생할 수 있으므로
 *    ThreadPool 워커로 넘겨(offload) 리액터 스레드가 멈추지 않도록 한다.
 *
 * [대규모 트래픽 대응]
 *  - 리액터 스레드에서는 이벤트 단위 로그를 남기지 않는다(디버그 모드 제외).
 *    대신 카운터를 모아 STATS_INTERVAL_SEC 마다 통계 한 줄만 출력한다.
 *  - SWEEP_INTERVAL_SEC 마다 IDLE_TIMEOUT_SEC 이상 무통신 연결을 정리한다.
 *    (클라이언트 배치 주기 10~60초 + 여유분을 고려한 값)
 *  - 워커 큐가 포화면 조회를 받지 않고 stale 캐시로 즉시 응답하거나
 *    폐기한다(load shedding). 폭증 시 지연이 무한히 자라는 것을 막는다.
 */
class EpollServer {
public:
    EpollServer(uint16_t              port,
                SpatialDensityEngine& engine,
                ZoneMapper&           zm,
                CongestionRouter&     cr,
                CacheManager&         cm,
                ThreadPool&           pool);
    ~EpollServer();

    void run();   // epoll 루프 시작 (stop() 호출 전까지 블로킹)
    void stop();  // 루프 종료 신호

private:
    void setNonBlocking(int fd);
    void handleAccept();
    void handleRead(ClientContext& ctx);
    void closeClient(int fd);
    void parseLine(ClientContext& ctx, std::string_view line);

    void sweepIdleClients();  // 유휴 연결 회수 (리액터 스레드에서 호출)
    void logStats();          // 주기 통계 출력 후 구간 카운터 리셋

    // 워커 스레드에서 부분 전송(EAGAIN/short write)을 처리하며 응답을 보낸다.
    static void sendAll(int fd, const char* data, size_t len);

    // 혼잡 레벨 → 클라이언트 권장 리포트 주기(초).
    // 밀집 존일수록 개별 리포트의 정보 가치가 낮으므로(존 리포트가 대체)
    // 주기를 늦춰 서버 유입량을 원격으로 감압한다.
    static int intervalHintFor(CongestionLevel level);

    int               listen_fd_ = -1;
    int               epoll_fd_  = -1;
    uint16_t          port_;
    std::atomic<bool> running_;

    MemoryPool                             pool_;     // zero-copy 수신 스크래치 버퍼 풀
    std::unordered_map<int, ClientContext> clients_;  // fd → 컨텍스트 (리액터 스레드 전용)

    // [B-1 추가] fd 별 generation 을 atomic 으로 별도 관리
    // clients_ 는 리액터 스레드 전용이지만 generation 은 ThreadPool 태스크에서도 읽으므로
    // atomic unordered_map 으로 분리하여 락 없이 안전하게 접근한다.
    std::unordered_map<int, std::atomic<uint64_t>> generations_;

    // 비즈니스 로직 컴포넌트 참조
    SpatialDensityEngine& densityEngine_;
    ZoneMapper&           zoneMapper_;
    CongestionRouter&     congestionRouter_;
    CacheManager&         cacheManager_;
    ThreadPool&           threadPool_;

    // ── 운영 카운터 (리액터 스레드 전용 — 락 불필요) ──
    uint64_t accepted_    = 0;
    uint64_t closed_      = 0;
    uint64_t idleClosed_  = 0;
    uint64_t lines_       = 0;
    uint64_t zoneReports_ = 0;
    uint64_t queries_     = 0;
    uint64_t shed_        = 0;
    std::chrono::steady_clock::time_point lastStatsAt_;
    std::chrono::steady_clock::time_point lastSweepAt_;

    static constexpr int MAX_EVENTS         = 1024;
    static constexpr int IDLE_TIMEOUT_SEC   = 90;  // 배치 3회(최대 60초 주기 고려) 미수신 시 회수
    static constexpr int SWEEP_INTERVAL_SEC = 30;
    static constexpr int STATS_INTERVAL_SEC = 10;
};

#endif // EPOLL_SERVER_H
