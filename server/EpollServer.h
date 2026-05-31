#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
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
 * [B-1 수정] generation 카운터 추가
 * fd 는 소켓이 닫힌 뒤 OS 가 동일 번호를 재사용할 수 있다.
 * ThreadPool 태스크가 이전 연결의 fd 로 send 하는 레이스를 막기 위해
 * closeClient() 때마다 generation 을 증가시키고,
 * 태스크 실행 직전에 현재 generation 과 비교한다.
 */
struct ClientContext {
    int         fd         = -1;
    uint64_t    generation = 0;  // [B-1 추가] fd 재사용 감지용 카운터
    std::string inbuf;           // 부분 수신된 줄을 모으는 per-connection 버퍼
};

/**
 * @brief epoll 기반 단일 리액터(reactor) 서버
 *
 *  - accept / recv 등 I/O 다중화는 epoll 단일 스레드가 담당한다.
 *  - 위치 갱신(userId != 0)은 SpatialDensityEngine 에 기록만 하고 응답하지 않는다(Silent Update).
 *  - 조회(userId == 0)는 외부 API 호출 등 블로킹이 발생할 수 있으므로
 *    ThreadPool 워커로 넘겨(offload) 리액터 스레드가 멈추지 않도록 한다.
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

    static constexpr int MAX_EVENTS = 1024;
};

#endif // EPOLL_SERVER_H