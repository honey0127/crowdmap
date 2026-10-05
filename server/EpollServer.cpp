#include "EpollServer.h"
#include "Logger.h"

#include <cstring>
#include <cerrno>
#include <algorithm>
#include <charconv>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>

EpollServer::EpollServer(uint16_t              port,
                         SpatialDensityEngine& engine,
                         ZoneMapper&           zm,
                         QueryService&         queries,
                         ClassPool&            simplePool,
                         ClassPool&            complexPool)
        : port_(port),
          running_(false),
          pool_(4096),                 // 8KB * 4096 = 32MB 수신 버퍼 풀 미리 확보
          densityEngine_(engine),
          zoneMapper_(zm),
          queries_(queries),
          simplePool_(simplePool),
          complexPool_(complexPool) {

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

    lastStatsAt_ = std::chrono::steady_clock::now();
    lastSweepAt_ = lastStatsAt_;

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

        // ── 주기 작업: epoll_wait 의 1초 타임아웃이 틱 역할을 한다 ──
        auto now = std::chrono::steady_clock::now();
        if (now - lastSweepAt_ >= std::chrono::seconds(SWEEP_INTERVAL_SEC)) {
            lastSweepAt_ = now;
            sweepIdleClients();
        }
        if (now - lastStatsAt_ >= std::chrono::seconds(STATS_INTERVAL_SEC)) {
            lastStatsAt_ = now;
            logStats();
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
        // 한 연결에 응답 여러 개가 연달아 나갈 때(요청이 여러 개 걸려 있거나 큐 적체가
        // 풀릴 때) Nagle 이 두 번째 응답을 앞 응답의 ACK 까지 붙잡아 지연이 수십 ms 늘어난다.
        // 요청-응답 프로토콜이라 작은 패킷을 모을 이유가 없다.
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = cfd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            close(cfd);
            continue;
        }

        auto& ctx = clients_[cfd];
        ctx.fd           = cfd;
        ctx.lastActivity = std::chrono::steady_clock::now();
        generations_[cfd].store(0);
        ++accepted_;
        Log::debug("[EpollServer] client connected fd=" + std::to_string(cfd));
    }
}

void EpollServer::closeClient(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);

    // [B-1] fd 닫을 때 generation 증가
    // → ThreadPool 태스크가 이전 연결의 fd 로 send 하는 레이스 방지
    if (generations_.count(fd)) generations_[fd].fetch_add(1);

    clients_.erase(fd);
    ++closed_;
    Log::debug("[EpollServer] client disconnected fd=" + std::to_string(fd));
}

// 셀룰러 NAT 뒤의 단말은 FIN 없이 사라진다. TCP keepalive 의 기본 감지는
// 2시간이라, 애플리케이션 차원에서 무통신 연결을 회수하지 않으면
// 축제급 트래픽에서 죽은 fd 가 무한히 누적되어 fd 한도가 고갈된다.
void EpollServer::sweepIdleClients() {
    auto now = std::chrono::steady_clock::now();

    std::vector<int> idle;
    for (auto& [fd, ctx] : clients_) {
        auto idleSec = std::chrono::duration_cast<std::chrono::seconds>(
            now - ctx.lastActivity).count();
        if (idleSec > IDLE_TIMEOUT_SEC) idle.push_back(fd);
    }
    for (int fd : idle) {
        ++idleClosed_;
        closeClient(fd);
    }
}

void EpollServer::logStats() {
    // 완전히 한가하면 침묵 (개발 콘솔 오염 방지)
    if (clients_.empty() && accepted_ == 0 && closed_ == 0 &&
        lines_ == 0 && querySimple_ == 0 && queryComplex_ == 0) {
        return;
    }
    auto poolStr = [](ClassPool& p) {
        return " " + p.name() + "(q=" + std::to_string(p.pending())
               + " c=" + std::to_string(p.active())
               + " K=" + std::to_string(p.cap()) + ")";
    };
    std::string pools = poolStr(simplePool_);
    if (&complexPool_ != &simplePool_) pools += poolStr(complexPool_);

    Log::info("[Stats] conns=" + std::to_string(clients_.size())
              + " accepted=" + std::to_string(accepted_)
              + " closed=" + std::to_string(closed_)
              + " idle_closed=" + std::to_string(idleClosed_)
              + " lines=" + std::to_string(lines_)
              + " zone_reports=" + std::to_string(zoneReports_)
              + " queries=" + std::to_string(querySimple_)
              + "/" + std::to_string(queryComplex_)
              + " shed=" + std::to_string(shed_)
              + pools);
    accepted_ = closed_ = idleClosed_ = 0;
    lines_ = zoneReports_ = querySimple_ = queryComplex_ = shed_ = 0;
}

void EpollServer::handleRead(ClientContext& ctx) {
    const int fd = ctx.fd;
    ctx.lastActivity = std::chrono::steady_clock::now();

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
    ++lines_;

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
            ++zoneReports_;
            Log::debug("[EpollServer] zone report: zone=" + std::to_string(zoneId)
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

    int      userId   = 0;
    double   lat = 0.0, lon = 0.0;
    int      bleCount = 0;
    uint32_t workUs   = 0;   // 실험 빌드: w=<µs> → COMPLEX
    uint64_t reqId    = 0;   // 실험 빌드: id=<n> → 응답에 그대로

    std::from_chars(user_id_sv.data(), user_id_sv.data() + user_id_sv.size(), userId);
    lat = std::atof(std::string(lat_sv).c_str());
    lon = std::atof(std::string(lon_sv).c_str());

    // 선택 필드: ",key=value" 반복 (ble=, w=, id=). 모르는 키는 무시한다.
    size_t pos = comma3;
    while (pos != std::string_view::npos) {
        const size_t next = line.find(',', pos + 1);
        std::string_view field = (next == std::string_view::npos)
                                 ? line.substr(pos + 1)
                                 : line.substr(pos + 1, next - pos - 1);
        pos = next;

        const size_t eq = field.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = field.substr(0, eq);
        const std::string_view val = field.substr(eq + 1);
        const char* b = val.data();
        const char* e = val.data() + val.size();
        if (key == "ble") {
            std::from_chars(b, e, bleCount);
        }
#ifdef CROWDMAP_EXPERIMENT
        // 클라이언트가 작업량을 정하는 것은 서비스 거부 공격에 쓰일 수 있어 실험 빌드에서만 받는다.
        else if (key == "w") {
            std::from_chars(b, e, workUs);
            workUs = std::min(workUs, QueryService::MAX_WORK_US);
        } else if (key == "id") {
            std::from_chars(b, e, reqId);
        }
#endif
    }

    // ── [Update] Silent Update ──
    if (userId != 0) {
        densityEngine_.recordLocation(userId, lat, lon, bleCount);
        return;
    }

    // ── [Query] 조회(userId == 0): 종류별 큐로 오프로드 ──
    const bool complex = workUs > 0;
    ++(complex ? queryComplex_ : querySimple_);
    const int zoneId = zoneMapper_.coordinateToZoneId(lat, lon);

    // 워커가 응답할 동안 리액터가 연결을 닫아 fd 번호가 재사용돼도 엉뚱한 연결로
    // 보내지 않도록, 작업마다 dup 한 fd 를 넘긴다(작업이 소유하고 응답 후 close).
    const int dupfd = dup(ctx.fd);
    if (dupfd < 0) {
        // fd 한도 소진(큐에 쌓인 작업마다 dup 한 fd 를 하나씩 쥐고 있다): 버리되 shed 로 응답해
        // 클라이언트·부하 생성기가 타임아웃이 아니라 shed 로 셀 수 있게 한다
        ++shed_;
        queries_.shedReply(ctx.fd, zoneId, reqId);
        return;
    }

    Job job;
    job.fd     = dupfd;
    job.zoneId = zoneId;
    job.lat    = lat;
    job.lon    = lon;
    job.workUs = workUs;
    job.reqId  = reqId;

    ClassPool& pool = complex ? complexPool_ : simplePool_;
    if (!pool.tryEnqueue(job)) {
        // ── Load shedding: 큐 길이가 입장 상한 K(t) 에 닿음 ──
        // 더 쌓으면 뒤 순번은 대기 상한(1초)을 넘긴다. stale 캐시가 있으면 그것으로
        // 즉시 응답하고(혼잡 상황이므로 interval=30 으로 감압 힌트),
        // 없으면 응답을 생략한다 — 클라이언트 타임아웃이 우아하게 처리.
        close(dupfd);
        ++shed_;
        queries_.shedReply(ctx.fd, zoneId, reqId);
    }
}
