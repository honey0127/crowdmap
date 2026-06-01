#include "EpollServer.h"
#include "Logger.h"

#include <cstring>
#include <cerrno>
#include <charconv>
#include <stdexcept>

EpollServer::EpollServer(uint16_t              port,
                         SpatialDensityEngine& engine,
                         ZoneMapper&           zm,
                         CongestionRouter&     cr,
                         CacheManager&         cm,
                         ThreadPool&           pool)
        : port_(port),
          running_(false),
          pool_(4096),                 // 8KB * 4096 = 32MB 수신 버퍼 풀 미리 확보
          densityEngine_(engine),
          zoneMapper_(zm),
          congestionRouter_(cr),
          cacheManager_(cm),
          threadPool_(pool) {

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket() 생성 실패");

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("bind() 실패 (port=" + std::to_string(port_) + ")");

    if (listen(listen_fd_, SOMAXCONN) < 0)
        throw std::runtime_error("listen() 실패");

    setNonBlocking(listen_fd_);

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) throw std::runtime_error("epoll_create1() 실패");

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);

    Log::info("[EpollServer] listening on port " + std::to_string(port_));
}

EpollServer::~EpollServer() {
    for (auto& [fd, ctx] : clients_) {
        close(fd);
    }
    if (epoll_fd_  >= 0) close(epoll_fd_);
    if (listen_fd_ >= 0) close(listen_fd_);
}

void EpollServer::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void EpollServer::stop() {
    running_ = false;
}

void EpollServer::run() {
    running_ = true;
    std::vector<epoll_event> events(MAX_EVENTS);

    while (running_) {
        // timeout 1000ms → stop() 신호를 최대 1초 안에 감지
        int n = epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            Log::err("[EpollServer] epoll_wait 실패");
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int      fd       = events[i].data.fd;
            const uint32_t evflags  = events[i].events;

            if (fd == listen_fd_) {
                handleAccept();
            } else if (evflags & (EPOLLHUP | EPOLLERR)) {
                closeClient(fd);
            } else if (evflags & EPOLLIN) {
                auto it = clients_.find(fd);
                if (it != clients_.end()) handleRead(it->second);
            }
        }
    }
}

void EpollServer::handleAccept() {
    // 논블로킹 listen 소켓이므로 EAGAIN 까지 모든 대기 연결을 수락
    while (true) {
        sockaddr_in caddr{};
        socklen_t   clen = sizeof(caddr);
        int cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);

        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 모두 수락 완료
            if (errno == EINTR) continue;
            Log::err("[EpollServer] accept 실패");
            break;
        }

        setNonBlocking(cfd);

        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = cfd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            close(cfd);
            continue;
        }

        clients_[cfd].fd = cfd;
        Log::info("[EpollServer] client connected fd=" + std::to_string(cfd));
    }
}

void EpollServer::closeClient(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    clients_.erase(fd);
    Log::info("[EpollServer] client disconnected fd=" + std::to_string(fd));
}

void EpollServer::handleRead(ClientContext& ctx) {
    const int fd = ctx.fd;

    // 논블로킹 소켓이므로 EAGAIN 까지 가능한 만큼 읽는다(레벨 트리거).
    while (true) {
        // MemoryPool 에서 8KB 청크를 빌려 커널 → 유저 공간 수신 버퍼로 사용
        void*   chunk = pool_.allocate();
        ssize_t got   = recv(fd, chunk, MemoryPool::CHUNK_SIZE, 0);

        if (got > 0) {
            ctx.inbuf.append(static_cast<char*>(chunk), static_cast<size_t>(got));
            pool_.deallocate(chunk);

            // 버퍼에서 '\n' 단위로 완성된 줄을 모두 추출
            size_t pos;
            while ((pos = ctx.inbuf.find('\n')) != std::string::npos) {
                size_t len = pos;
                if (len > 0 && ctx.inbuf[len - 1] == '\r') --len;  // CRLF 보정
                parseLine(ctx, std::string_view(ctx.inbuf.data(), len));
                ctx.inbuf.erase(0, pos + 1);
            }
            // got < CHUNK_SIZE 여도 곧장 종료하지 않고 한 번 더 recv → EAGAIN 으로 확실히 비움
        } else if (got == 0) {
            pool_.deallocate(chunk);
            closeClient(fd);   // 정상 종료(상대가 FIN)
            return;
        } else { // got < 0
            pool_.deallocate(chunk);
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // 더 읽을 것 없음
            if (errno == EINTR) continue;
            closeClient(fd);   // 실제 오류
            return;
        }
    }
}

void EpollServer::parseLine(ClientContext& ctx, std::string_view line) {
    if (line.empty()) return;

    size_t comma1 = line.find(',');
    size_t comma2 = line.find(',', comma1 + 1);
    if (comma1 == std::string_view::npos || comma2 == std::string_view::npos) return;

    std::string_view user_id_sv = line.substr(0, comma1);
    std::string_view lat_sv     = line.substr(comma1 + 1, comma2 - comma1 - 1);

    // 세 번째 필드 이후에 선택적 BLE 필드가 올 수 있음
    // 형식: "userId,lat,lon" 또는 "userId,lat,lon,ble=N"
    size_t comma3 = line.find(',', comma2 + 1);
    std::string_view lon_sv = (comma3 == std::string_view::npos)
                              ? line.substr(comma2 + 1)
                              : line.substr(comma2 + 1, comma3 - comma2 - 1);

    int    userId   = 0;
    double lat = 0.0, lon = 0.0;
    int    bleCount = 0;

    std::from_chars(user_id_sv.data(), user_id_sv.data() + user_id_sv.size(), userId);
    lat = std::atof(std::string(lat_sv).c_str());
    lon = std::atof(std::string(lon_sv).c_str());

    // "ble=N" 필드 파싱 (존재하는 경우)
    if (comma3 != std::string_view::npos) {
        std::string_view ble_sv = line.substr(comma3 + 1);
        constexpr std::string_view BLE_PREFIX = "ble=";
        if (ble_sv.substr(0, BLE_PREFIX.size()) == BLE_PREFIX) {
            std::string_view ble_val = ble_sv.substr(BLE_PREFIX.size());
            std::from_chars(ble_val.data(), ble_val.data() + ble_val.size(), bleCount);
        }
    }

    // ── [Update] 위치 갱신: 밀도 엔진에 기록만, 응답 없음(Silent Update) ──
    if (userId != 0) {
        densityEngine_.recordLocation(userId, lat, lon, bleCount);
        return;
    }

    // ── [Query] 조회(userId == 0): 블로킹 가능성이 있으므로 ThreadPool 로 오프로드 ──
    // this 가 아니라 의존 컴포넌트들을 포인터로 캡처한다.
    // (리액터/서버 객체보다 이 컴포넌트들이 더 오래 살아남도록 main 에서 수명을 보장)
    const int             fd      = ctx.fd;
    SpatialDensityEngine* engine  = &densityEngine_;
    ZoneMapper*           zm      = &zoneMapper_;
    CongestionRouter*     router  = &congestionRouter_;
    CacheManager*         cache   = &cacheManager_;

    threadPool_.enqueue([fd, lat, lon, engine, zm, router, cache]() {
        // 클릭 지점 주변 3x3 그리드의 실시간 밀도(로컬 + 가상 유저)
        size_t localDensity = engine->getDensity(lat, lon);

        int zoneId = zm->coordinateToZoneId(lat, lon);
        std::string response;

        if (zoneId == -1) {
            response = "RELAXED|0.0\n";
        } else {
            CongestionResult result;
            if (!cache->get(zoneId, result)) {
                result = router->resolve(lat, lon,
                                         static_cast<int>(localDensity),
                                         *cache, zoneId);
            }
            response = std::string(result.levelString()) + "|"
                       + std::to_string(result.ratio) + "\n";
        }

        // MSG_NOSIGNAL: 상대가 이미 닫은 소켓에 보내도 SIGPIPE 로 죽지 않음
        send(fd, response.c_str(), response.length(), MSG_NOSIGNAL);
    });
}