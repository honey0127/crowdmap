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
    for (auto& [fd, ctx] : clients_) close(fd);
    if (epoll_fd_  >= 0) close(epoll_fd_);
    if (listen_fd_ >= 0) close(listen_fd_);
}

void EpollServer::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void EpollServer::stop() { running_ = false; }

void EpollServer::run() {
    running_ = true;
    std::vector<epoll_event> events(MAX_EVENTS);

    while (running_) {
        int n = epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            Log::err("[EpollServer] epoll_wait 실패");
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int      fd      = events[i].data.fd;
            const uint32_t evflags = events[i].events;

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
    while (true) {
        sockaddr_in caddr{};
        socklen_t   clen = sizeof(caddr);
        int cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);

        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
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
        generations_[cfd].store(0);  // [B-1] 새 연결 시 generation 초기화
        Log::info("[EpollServer] client connected fd=" + std::to_string(cfd));
    }
}

void EpollServer::closeClient(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);

    // [B-1] fd 닫을 때 generation 증가
    // → ThreadPool 태스크가 이전 연결의 fd 로 send 하는 레이스 방지
    if (generations_.count(fd)) generations_[fd].fetch_add(1);

    clients_.erase(fd);
    Log::info("[EpollServer] client disconnected fd=" + std::to_string(fd));
}

void EpollServer::handleRead(ClientContext& ctx) {
    const int fd = ctx.fd;

    while (true) {
        void*   chunk = pool_.allocate();
        ssize_t got   = recv(fd, chunk, MemoryPool::CHUNK_SIZE, 0);

        if (got > 0) {
            ctx.inbuf.append(static_cast<char*>(chunk), static_cast<size_t>(got));
            pool_.deallocate(chunk);

            // [B-2] inbuf 64KB 상한 — DoS 방지
            if (ctx.inbuf.size() > 65536) {
                Log::warn("[EpollServer] inbuf overflow fd=" + std::to_string(fd)
                          + " size=" + std::to_string(ctx.inbuf.size()) + " → 연결 종료");
                closeClient(fd);
                return;
            }

            // [B-7] 파싱 루프 32줄 상한 — 리액터 독점 방지
            size_t pos;
            size_t parsedLines = 0;
            while (parsedLines < 32 &&
                   (pos = ctx.inbuf.find('\n')) != std::string::npos) {
                size_t len = pos;
                if (len > 0 && ctx.inbuf[len - 1] == '\r') --len;
                parseLine(ctx, std::string_view(ctx.inbuf.data(), len));
                ctx.inbuf.erase(0, pos + 1);
                ++parsedLines;
            }
        } else if (got == 0) {
            pool_.deallocate(chunk);
            closeClient(fd);
            return;
        } else {
            pool_.deallocate(chunk);
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            closeClient(fd);
            return;
        }
    }
}

void EpollServer::parseLine(ClientContext& ctx, std::string_view line) {
    if (line.empty()) return;

    // ── [Zone Density Report] P2P 집계 메시지: "zone=<id>,density=<N>" ──
    constexpr std::string_view ZONE_PREFIX = "zone=";
    if (line.substr(0, ZONE_PREFIX.size()) == ZONE_PREFIX) {
        size_t dComma = line.find(',', ZONE_PREFIX.size());
        if (dComma == std::string_view::npos) return;

        std::string_view zoneId_sv  = line.substr(ZONE_PREFIX.size(), dComma - ZONE_PREFIX.size());
        std::string_view density_sv = line.substr(dComma + 1);

        constexpr std::string_view DENSITY_PREFIX = "density=";
        if (density_sv.substr(0, DENSITY_PREFIX.size()) != DENSITY_PREFIX) return;
        density_sv = density_sv.substr(DENSITY_PREFIX.size());

        int zoneId = -1, density = 0;
        std::from_chars(zoneId_sv.data(),  zoneId_sv.data()  + zoneId_sv.size(),  zoneId);
        std::from_chars(density_sv.data(), density_sv.data() + density_sv.size(), density);

        if (zoneId >= 0 && density > 0) {
            densityEngine_.recordZoneDensity(zoneId, density);
            Log::info("[EpollServer] zone report: zone=" + std::to_string(zoneId)
                      + " density=" + std::to_string(density));
        }
        return;
    }

    size_t comma1 = line.find(',');
    size_t comma2 = line.find(',', comma1 + 1);
    if (comma1 == std::string_view::npos || comma2 == std::string_view::npos) return;

    std::string_view user_id_sv = line.substr(0, comma1);
    std::string_view lat_sv     = line.substr(comma1 + 1, comma2 - comma1 - 1);

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

    if (comma3 != std::string_view::npos) {
        std::string_view ble_sv = line.substr(comma3 + 1);
        constexpr std::string_view BLE_PREFIX = "ble=";
        if (ble_sv.substr(0, BLE_PREFIX.size()) == BLE_PREFIX) {
            std::string_view ble_val = ble_sv.substr(BLE_PREFIX.size());
            std::from_chars(ble_val.data(), ble_val.data() + ble_val.size(), bleCount);
        }
    }

    // ── [Update] Silent Update ──
    if (userId != 0) {
        densityEngine_.recordLocation(userId, lat, lon, bleCount);
        return;
    }

    // ── [Query] ThreadPool 오프로드 ──
    const int             fd         = ctx.fd;
    const uint64_t        generation = generations_.count(fd)
                                       ? generations_[fd].load() : 0;  // [B-1] generation 캡처
    SpatialDensityEngine* engine     = &densityEngine_;
    ZoneMapper*           zm         = &zoneMapper_;
    CongestionRouter*     router     = &congestionRouter_;
    CacheManager*         cache      = &cacheManager_;
    auto*                 gens       = &generations_;

    threadPool_.enqueue([fd, generation, lat, lon, engine, zm, router, cache, gens]() {
        size_t localDensity = engine->getDensity(lat, lon);

        int zoneId = zm->coordinateToZoneId(lat, lon);
        std::string response;

        if (zoneId == -1) {
            response = "RELAXED|0.0\n";
        } else {
            int    zoneReport      = engine->getZoneReport(zoneId);
            size_t effectiveDensity = (zoneReport > 0)
                ? std::max(localDensity, static_cast<size_t>(zoneReport))
                : localDensity;

            CongestionResult result;
            if (!cache->get(zoneId, result)) {
                result = router->resolve(lat, lon,
                                         static_cast<int>(effectiveDensity),
                                         *cache, zoneId);
            }
            response = std::string(result.levelString()) + "|"
                       + std::to_string(result.ratio) + "\n";
        }

        // [B-1] send 직전 generation 검증 — fd 재사용 감지
        if (gens->count(fd) && (*gens)[fd].load() != generation) return;

        send(fd, response.c_str(), response.length(), MSG_NOSIGNAL);
    });
}