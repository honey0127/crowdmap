#include "EpollServer.h"
#include "Logger.h"

#include <cstring>
#include <cerrno>
#include <charconv>
#include <poll.h>
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

    lastStatsAt_ = std::chrono::steady_clock::now();
    lastSweepAt_ = lastStatsAt_;

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
        // timeout 1000ms → stop() 신호와 주기 작업(통계/유휴 정리)을 최대 1초 안에 처리
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

        auto& ctx = clients_[cfd];
        ctx.fd           = cfd;
        ctx.lastActivity = std::chrono::steady_clock::now();
        ++accepted_;
        // 연결 단위 로그는 핫패스 — 폭증 시 리액터가 터미널 I/O에 직렬화되므로
        // 디버그 모드에서만 출력하고 평시에는 주기 통계로 대체한다.
        Log::debug("[EpollServer] client connected fd=" + std::to_string(cfd));
    }
}

void EpollServer::closeClient(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
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
        lines_ == 0 && queries_ == 0) {
        return;
    }
    Log::info("[Stats] conns=" + std::to_string(clients_.size())
              + " accepted=" + std::to_string(accepted_)
              + " closed=" + std::to_string(closed_)
              + " idle_closed=" + std::to_string(idleClosed_)
              + " lines=" + std::to_string(lines_)
              + " zone_reports=" + std::to_string(zoneReports_)
              + " queries=" + std::to_string(queries_)
              + " shed=" + std::to_string(shed_)
              + " pool_pending=" + std::to_string(threadPool_.pending()));
    accepted_ = closed_ = idleClosed_ = 0;
    lines_ = zoneReports_ = queries_ = shed_ = 0;
}

void EpollServer::handleRead(ClientContext& ctx) {
    const int fd = ctx.fd;
    ctx.lastActivity = std::chrono::steady_clock::now();

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

// 워커 스레드용 송신 헬퍼: 논블로킹 소켓의 부분 전송(short write)과
// EAGAIN 을 처리한다. 응답이 짧아(수십 바이트) 보통 한 번에 끝나지만,
// 송신 버퍼가 가득 찬 느린 클라이언트에서는 잠시(최대 300ms) 기다렸다
// 재시도하고, 그래도 안 되면 폐기한다 — 느린 소비자가 워커를 오래
// 붙잡으면 그 자체가 새로운 병목이 되기 때문이다.
void EpollServer::sendAll(int fd, const char* data, size_t len) {
    size_t off   = 0;
    int    waits = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits < 3) {
            pollfd p{};
            p.fd     = fd;
            p.events = POLLOUT;
            ::poll(&p, 1, 100);
            ++waits;
            continue;
        }
        return;  // 죽었거나 너무 느린 클라이언트: 응답 폐기 (클라이언트 타임아웃이 처리)
    }
}

int EpollServer::intervalHintFor(CongestionLevel level) {
    switch (level) {
        case CongestionLevel::CROWDED:  return 30;
        case CongestionLevel::MODERATE: return 15;
        default:                        return 10;
    }
}

void EpollServer::parseLine(ClientContext& ctx, std::string_view line) {
    if (line.empty()) return;
    ++lines_;

    // ── [Zone Density Report] P2P 집계 메시지: "zone=<id>,density=<N>" ──
    // 기기들이 Nearby Connections 없이도 BLE 카운트를 존 단위로 집계해 전송한다.
    // 서버 커넥션 수를 1/N으로 줄이는 핵심 경량 포맷.
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
    ++queries_;
    const int zoneId = zoneMapper_.coordinateToZoneId(lat, lon);

    // [fd 재사용 레이스 방지] 워커가 응답을 보내기 전에 이 클라이언트가 끊기면
    // 리액터가 fd 를 닫고, 새 accept 가 같은 번호를 재사용할 수 있다.
    // 그 상태에서 워커가 원래 fd 번호로 send 하면 응답이 "엉뚱한 클라이언트"에게 간다.
    // dup() 은 리액터 스레드에서(=closeClient 와 경합 없이) 호출되므로 안전하고,
    // 복제된 디스크립터는 원본이 닫혀도 기존 소켓을 계속 가리킨다.
    const int dupfd = dup(ctx.fd);
    if (dupfd < 0) {
        ++shed_;
        return;  // fd 고갈: 조회 폐기 (클라이언트 타임아웃이 처리)
    }

    // this 가 아니라 의존 컴포넌트들을 포인터로 캡처한다.
    // (리액터/서버 객체보다 이 컴포넌트들이 더 오래 살아남도록 main 에서 수명을 보장)
    SpatialDensityEngine* engine  = &densityEngine_;
    CongestionRouter*     router  = &congestionRouter_;
    CacheManager*         cache   = &cacheManager_;

    const bool queued = threadPool_.enqueue([dupfd, lat, lon, zoneId, engine, router, cache]() {
        std::string response;

        if (zoneId == -1) {
            response = "RELAXED|0.0|interval=10\n";
        } else {
            // 클릭 지점 주변 3x3 그리드의 실시간 밀도(GPS + BLE 보정 포함)
            size_t localDensity = engine->getDensity(lat, lon);

            // P2P 집계 리포트가 있으면 GPS 밀도와 비교해 더 큰 값을 사용
            // (dense zone에서 GPS 카운트가 과소 추정될 때 zone report로 보정)
            int zoneReport = engine->getZoneReport(zoneId);
            size_t effectiveDensity = (zoneReport > 0)
                ? std::max(localDensity, static_cast<size_t>(zoneReport))
                : localDensity;

            // 캐시 확인/single-flight/negative cache 는 router 가 일원화해 처리
            CongestionResult result = router->resolve(lat, lon,
                                                      static_cast<int>(effectiveDensity),
                                                      *cache, zoneId);

            // interval 힌트: 혼잡할수록 클라이언트 리포트 주기를 늦춰
            // 서버 유입량을 원격으로 줄인다 (클라이언트는 자기 위치 조회
            // 응답의 힌트만 배치 주기에 반영한다)
            response = std::string(result.levelString()) + "|"
                       + std::to_string(result.ratio)
                       + "|interval=" + std::to_string(intervalHintFor(result.level))
                       + "\n";
        }

        sendAll(dupfd, response.c_str(), response.length());
        close(dupfd);
    });

    if (!queued) {
        // ── Load shedding: 워커 큐 포화 ──
        // 새 작업을 쌓으면 지연만 폭발한다. stale 캐시가 있으면 그것으로
        // 즉시 응답하고(혼잡 상황이므로 interval=30 으로 감압 힌트),
        // 없으면 응답을 생략한다 — 클라이언트 타임아웃이 우아하게 처리.
        close(dupfd);
        ++shed_;

        CongestionResult stale;
        if (zoneId != -1 && cacheManager_.getStale(zoneId, stale, 300)) {
            std::string resp = std::string(stale.levelString()) + "|"
                               + std::to_string(stale.ratio) + "|interval=30\n";
            // 리액터 스레드: 논블로킹 1회 시도만. 안 나가면 폐기.
            ::send(ctx.fd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        }
    }
}
