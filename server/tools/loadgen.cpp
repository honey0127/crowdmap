// CrowdMap v5 부하 생성기 — 실험 빌드 서버(-DCROWDMAP_EXPERIMENT=ON)용
//
//  - 열린 루프(open loop): 응답을 기다리지 않고 예정 시각에 보낸다. 지연은 예정 시각 → 수신으로
//    잰다(생성기가 밀려도 지연이 작게 찍히지 않게 — coordinated omission 방지).
//  - 위치 업데이트는 보내지 않는다(조회만). 워커 풀만 떼어 보기 위해서다.
//  - 도착 = 포아송(1−f) + 동기화(f): 동기화 몫은 주기 P 마다 폭 w 안에 균등하게 몰린다.
//    순간 최대/평균 = f·P/w + (1 − f)  (f = 0.2, P = 25, w = 3 → 2.47)
//    클라이언트마다 간격 표준편차를 키우는 방식은 쓰지 않는다: 독립 클라이언트가 많으면
//    합쳐진 도착이 포아송에 가까워져(Palm–Khintchine) 서버에서 차이가 거의 안 보인다.
//  - 요청 종류: 확률 p 로 COMPLEX(w=<µs>, 고정 또는 지수분포), 나머지 SIMPLE.
//  - 응답은 id 로 맞춘다(서버는 종류별 큐라 순서가 바뀔 수 있다).
//
// 예) 이용률 0.8(1코어 기준), COMPLEX 가 CPU 50%, 연결 20% 동기화:
//   crowdmap_loadgen --host 10.0.0.2 --rho 0.8 --complex-share 0.5 --sync-frac 0.2 --out run1
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// ── 설정 ────────────────────────────────────────────────────────────────
struct Config {
    std::string host        = "127.0.0.1";
    int         port        = 8765;
    int         conns       = 20000;   // 연결 8개 × 2만 = 16만 > 최대 유입(초당 60k × 1초)의 2.7배
    int         threads     = 4;
    int         outstanding = 8;       // 연결당 동시에 걸어 둘 수 있는 요청 수
    double      rate        = 0;       // 평균 도착률(/s). 0 이면 rho 로 계산
    double      rho         = 0;       // 1코어(×cores) 기준 이용률
    double      cores       = 1;
    double      sSimpleUs   = 30;      // SIMPLE 처리 시간(실험 0 보정값)
    double      complexFrac = -1;      // COMPLEX 비율 p
    double      complexShare = 0;      // 또는 COMPLEX 가 차지하는 CPU 비율 → p 계산
    double      workUs      = 10000;   // COMPLEX 작업량
    bool        workExp     = false;   // false: 고정, true: 지수분포
    double      syncFrac    = 0;       // f
    double      syncWidth   = 3.0;     // w (v4 실측)
    double      syncPeriod  = 25.0;    // P (v4 실측)
    double      syncOffset  = 0.0;     // 첫 몰림 시작(시작 기준 초)
    double      warmup      = 10;
    double      duration    = 75;      // 버스트 주기 25초의 3배
    double      timeout     = 3.0;     // 앱 READ_TIMEOUT
    double      qThreshMs   = 50;      // 서버 큐 대기가 이 값을 넘은 비율을 센다 (대기 목표)
    double      minConnFrac = 0.99;    // 연결이 이 비율보다 적게 되면 부하를 걸지 않고 종료(코드 3)
    int         zones       = 5;
    uint64_t    seed        = 1;
    std::string out;                   // 결과 파일 접두어
    std::string label;
};

void usage() {
    std::fprintf(stderr,
        "usage: crowdmap_loadgen [옵션]\n"
        "  --host H --port P            서버 (127.0.0.1:8765)\n"
        "  --conns N --threads T        연결 수(20000), 생성 스레드(4)\n"
        "  --outstanding K              연결당 동시 요청 상한(8)\n"
        "  --rate R | --rho X           평균 도착률(/s) 또는 이용률(1코어 기준)\n"
        "  --cores C --s-simple-us U    rho 계산용: 코어 수(1), SIMPLE 처리시간 µs(30)\n"
        "  --complex-frac p | --complex-share x   COMPLEX 비율 또는 CPU 점유율\n"
        "  --work-us W --work-dist fixed|exp      COMPLEX 작업량(10000µs), 분포(fixed)\n"
        "  --sync-frac f --sync-width w --sync-period P --sync-offset o   몰림(0, 3, 25, 0)\n"
        "  --warmup s --duration s --timeout s    (10, 75, 3)\n"
        "  --q-threshold-ms T           q > T 인 응답 수를 따로 셈(50)\n"
        "  --min-conn-frac F            연결 성공률이 F 미만이면 종료 코드 3(0.99)\n"
        "  --zones n --seed n --out prefix --label name\n");
}

bool parseArgs(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 값 없음\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") c.host = next("--host");
        else if (a == "--port") c.port = std::atoi(next("--port"));
        else if (a == "--conns") c.conns = std::atoi(next("--conns"));
        else if (a == "--threads") c.threads = std::atoi(next("--threads"));
        else if (a == "--outstanding") c.outstanding = std::atoi(next("--outstanding"));
        else if (a == "--rate") c.rate = std::atof(next("--rate"));
        else if (a == "--rho") c.rho = std::atof(next("--rho"));
        else if (a == "--cores") c.cores = std::atof(next("--cores"));
        else if (a == "--s-simple-us") c.sSimpleUs = std::atof(next("--s-simple-us"));
        else if (a == "--complex-frac") c.complexFrac = std::atof(next("--complex-frac"));
        else if (a == "--complex-share") c.complexShare = std::atof(next("--complex-share"));
        else if (a == "--work-us") c.workUs = std::atof(next("--work-us"));
        else if (a == "--work-dist") c.workExp = std::string(next("--work-dist")) == "exp";
        else if (a == "--sync-frac") c.syncFrac = std::atof(next("--sync-frac"));
        else if (a == "--sync-width") c.syncWidth = std::atof(next("--sync-width"));
        else if (a == "--sync-period") c.syncPeriod = std::atof(next("--sync-period"));
        else if (a == "--sync-offset") c.syncOffset = std::atof(next("--sync-offset"));
        else if (a == "--warmup") c.warmup = std::atof(next("--warmup"));
        else if (a == "--duration") c.duration = std::atof(next("--duration"));
        else if (a == "--timeout") c.timeout = std::atof(next("--timeout"));
        else if (a == "--q-threshold-ms") c.qThreshMs = std::atof(next("--q-threshold-ms"));
        else if (a == "--min-conn-frac") c.minConnFrac = std::atof(next("--min-conn-frac"));
        else if (a == "--zones") c.zones = std::atoi(next("--zones"));
        else if (a == "--seed") c.seed = std::strtoull(next("--seed"), nullptr, 10);
        else if (a == "--out") c.out = next("--out");
        else if (a == "--label") c.label = next("--label");
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "알 수 없는 옵션: %s\n", a.c_str());
            usage();
            return false;
        }
    }
    if (c.complexFrac < 0) {
        // CPU 점유율 x → 비율 p:  p·W / ((1−p)·S + p·W) = x
        const double x = std::clamp(c.complexShare, 0.0, 0.999);
        c.complexFrac  = x * c.sSimpleUs / (x * c.sSimpleUs + (1 - x) * c.workUs);
    }
    if (c.rate <= 0 && c.rho > 0) {
        const double es = (1 - c.complexFrac) * c.sSimpleUs + c.complexFrac * c.workUs;
        c.rate = c.rho * c.cores / (es * 1e-6);
    }
    if (c.rate <= 0) {
        std::fprintf(stderr, "--rate 또는 --rho 가 필요합니다\n");
        return false;
    }
    c.threads     = std::max(1, std::min(c.threads, c.conns));
    c.outstanding = std::max(1, c.outstanding);
    c.zones       = std::clamp(c.zones, 1, 5);
    c.syncFrac    = std::clamp(c.syncFrac, 0.0, 1.0);
    return true;
}

int64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// ── 로그-선형 히스토그램 (µs, 상대 오차 약 1.6%) ─────────────────────────
struct Hist {
    static constexpr int kSub  = 64;
    static constexpr int kSize = 2 * kSub + kSub * (64 - 7);
    std::vector<uint64_t> b = std::vector<uint64_t>(kSize, 0);
    uint64_t n   = 0;
    double   sum = 0;
    uint64_t max = 0;

    static int idx(uint64_t v) {
        if (v < 2 * kSub) return static_cast<int>(v);
        const int e = 63 - __builtin_clzll(v);   // v ≥ 128 → e ≥ 7
        return 2 * kSub + (e - 7) * kSub + static_cast<int>((v >> (e - 6)) & (kSub - 1));
    }
    static double mid(int i) {
        if (i < 2 * kSub) return i;
        const int    k = i - 2 * kSub;
        const int    e = k / kSub + 7;
        const double lo = static_cast<double>((uint64_t(kSub) + k % kSub) << (e - 6));
        return lo + static_cast<double>(uint64_t(1) << (e - 6)) / 2.0;
    }
    void add(int64_t us) {
        const uint64_t v = us < 0 ? 0 : static_cast<uint64_t>(us);
        ++b[idx(v)];
        ++n;
        sum += static_cast<double>(v);
        max = std::max(max, v);
    }
    void merge(const Hist& o) {
        for (int i = 0; i < kSize; ++i) b[i] += o.b[i];
        n += o.n;
        sum += o.sum;
        max = std::max(max, o.max);
    }
    double pct(double p) const {   // µs
        if (n == 0) return 0;
        const uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(n)));
        uint64_t       acc    = 0;
        for (int i = 0; i < kSize; ++i) {
            acc += b[i];
            if (acc >= target) return std::min(mid(i), static_cast<double>(max));
        }
        return static_cast<double>(max);
    }
};

enum Cls { SIMPLE = 0, COMPLEX = 1 };

struct ClassStats {
    uint64_t sent = 0, ok = 0, shed = 0, late = 0, timeout = 0, skipped = 0, lost = 0;
    uint64_t qOver = 0;   // 정상 처리 중 서버 큐 대기 > --q-threshold-ms
    Hist     lat;     // 응답한 전부(ok + shed + late): 예정 송신 → 수신
    Hist     latOk;   // 정상 처리만
    Hist     q;       // 서버 큐 대기 (ok)
    Hist     s;       // 서버 처리 (ok)
    Hist     rest;    // 응답 지연 − q − s (ok): 네트워크 + 리액터 + 생성기. 리액터 포화 판정용
    void merge(const ClassStats& o) {
        sent += o.sent; ok += o.ok; shed += o.shed; late += o.late;
        timeout += o.timeout; skipped += o.skipped; lost += o.lost; qOver += o.qOver;
        lat.merge(o.lat); latOk.merge(o.latOk); q.merge(o.q); s.merge(o.s);
        rest.merge(o.rest);
    }
};

constexpr double kBinSec   = 0.1;    // 시계열 칸
constexpr double kFineSec  = 0.01;   // 분산지수용 칸

struct Bin {
    uint64_t sent[2] = {0, 0}, ok[2] = {0, 0}, shed = 0, late = 0, timeout = 0;
    double   latSum[2] = {0, 0};
    uint64_t latMax[2] = {0, 0};
    uint64_t qMax      = 0;
};

struct ThreadResult {
    ClassStats cls[2];
    Hist       genLag;              // 실제 송신 − 예정 송신
    std::vector<Bin>      bins;
    std::vector<uint32_t> fine;     // 10ms 칸별 예정 송신 수 (분산지수)
    uint64_t unknownId = 0, disconnects = 0, connected = 0;
};

// ── 연결 ────────────────────────────────────────────────────────────────
struct Pending {
    uint64_t id     = 0;   // 0 = 빈 칸
    int64_t  schedNs = 0;
    uint8_t  cls    = 0;
    bool     measured = false;
    int      bin    = -1;
};

struct Conn {
    int                  fd = -1;
    bool                 up = false;
    int                  free = 0;
    std::vector<Pending> pend;
    std::string          in;
    std::string          out;
};

// 서울 5개 지점 (외부 API covers 범위·ZoneMapper 범위 안)
const double kZones[5][2] = {
    {37.4979, 127.0276},   // 강남
    {37.5563, 126.9220},   // 홍대
    {37.5636, 126.9869},   // 명동
    {37.5133, 127.1001},   // 잠실
    {37.5219, 126.9245},   // 여의도
};

class Worker {
public:
    Worker(const Config& c, int id, int connFrom, int connTo)
        : c_(c), id_(id), rng_(c.seed * 1000003ULL + static_cast<uint64_t>(id)) {
        conns_.resize(static_cast<size_t>(connTo - connFrom));
        for (auto& cn : conns_) {
            cn.pend.resize(static_cast<size_t>(c.outstanding));
            cn.free = c.outstanding;
        }
        const double total = c.warmup + c.duration;
        res_.bins.resize(static_cast<size_t>(std::ceil(total / kBinSec)) + 1);
        res_.fine.resize(static_cast<size_t>(std::ceil(total / kFineSec)) + 1);
        rate_ = c.rate / c.threads;
    }

    // 연결 수립 (모든 스레드가 끝날 때까지 main 이 기다린다)
    void connectAll(std::atomic<int>& readyCount) {
        ep_ = epoll_create1(0);
        tfd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        epoll_event ev{};
        ev.events   = EPOLLIN;
        ev.data.u64 = UINT64_MAX;
        epoll_ctl(ep_, EPOLL_CTL_ADD, tfd_, &ev);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(c_.port));
        inet_pton(AF_INET, c_.host.c_str(), &addr.sin_addr);
        addr_ = addr;

        const int64_t deadline = nowNs() + 60'000'000'000LL;
        size_t next = 0;
        int inflight = 0;
        while ((next < conns_.size() || inflight > 0) && nowNs() < deadline) {
            while (next < conns_.size() && inflight < 500) {   // 동시 connect 500개씩
                if (startConnect(next)) ++inflight;
                ++next;
            }
            epoll_event evs[512];
            const int n = epoll_wait(ep_, evs, 512, 100);
            for (int i = 0; i < n; ++i) {
                const uint64_t k = evs[i].data.u64;
                if (k == UINT64_MAX) continue;
                Conn& cn = conns_[k];
                if (cn.up) continue;
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(cn.fd, SOL_SOCKET, SO_ERROR, &err, &len);
                --inflight;
                if (err != 0 || (evs[i].events & (EPOLLERR | EPOLLHUP))) {
                    ::close(cn.fd);
                    cn.fd = -1;
                    continue;
                }
                cn.up = true;
                epoll_event e{};
                e.events   = EPOLLIN | EPOLLRDHUP;
                e.data.u64 = k;
                epoll_ctl(ep_, EPOLL_CTL_MOD, cn.fd, &e);
            }
        }
        for (auto& cn : conns_) res_.connected += cn.up ? 1 : 0;
        readyCount.fetch_add(1);
    }

    void run(int64_t t0Ns) {
        t0_      = t0Ns;
        endSend_ = t0Ns + static_cast<int64_t>((c_.warmup + c_.duration) * 1e9);
        mStart_  = t0Ns + static_cast<int64_t>(c_.warmup * 1e9);
        expD_    = std::exponential_distribution<double>(1.0);

        prate_ = rate_ * (1.0 - c_.syncFrac);
        nextPoisson_ = prate_ > 0 ? expD_(rng_) / prate_ : 1e18;
        syncPeriodIdx_ = -1;
        refillSync();

        std::vector<epoll_event> evs(1024);
        int64_t lastScan = nowNs();
        int64_t armedAt  = -1;
        bool    draining = false;
        const int64_t drainEnd = endSend_ + static_cast<int64_t>((c_.timeout + 0.5) * 1e9);

        while (true) {
            int64_t now = nowNs();
            if (!draining && now >= endSend_) draining = true;

            if (!draining) {
                int64_t due = nextArrivalNs();
                while (due <= now && due < endSend_) {
                    issue(due, now);
                    advanceArrival();
                    due = nextArrivalNs();
                    now = nowNs();
                }
                if (due < endSend_ && due != armedAt) {
                    itimerspec its{};
                    its.it_value.tv_sec  = due / 1'000'000'000LL;
                    its.it_value.tv_nsec = due % 1'000'000'000LL;
                    timerfd_settime(tfd_, TFD_TIMER_ABSTIME, &its, nullptr);
                    armedAt = due;
                }
            }

            if (now - lastScan >= 100'000'000LL) {   // 타임아웃 검사 100ms 마다
                scanTimeouts(now);
                lastScan = now;
            }
            if (draining && (outstandingTotal_ == 0 || now >= drainEnd)) break;

            const int n = epoll_wait(ep_, evs.data(), static_cast<int>(evs.size()), 100);
            for (int i = 0; i < n; ++i) {
                const uint64_t k = evs[i].data.u64;
                if (k == UINT64_MAX) {
                    uint64_t x;
                    while (::read(tfd_, &x, sizeof(x)) > 0) {}
                    continue;
                }
                Conn& cn = conns_[k];
                if (!cn.up) {   // 재연결 중인 소켓
                    if (cn.fd >= 0) finishConnect(k, evs[i].events);
                    continue;
                }
                if (evs[i].events & EPOLLIN) readConn(k);
                if (cn.up && (evs[i].events & EPOLLOUT)) flushConn(k);
                if (cn.up && (evs[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))) dropConn(k);
            }
        }
        // 남은 요청은 타임아웃 처리
        scanTimeouts(INT64_MAX);
        // RST 로 닫는다: FIN 으로 닫으면 연결마다 TIME_WAIT 가 60초 남아, 바로 이어지는
        // 다음 실행이 임시 포트(기본 약 2.8만 개)를 다 못 얻는다.
        const linger lg{1, 0};
        for (auto& cn : conns_) {
            if (cn.fd < 0) continue;
            setsockopt(cn.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
            ::close(cn.fd);
        }
        ::close(tfd_);
        ::close(ep_);
    }

    ThreadResult& result() { return res_; }

private:
    bool startConnect(size_t k) {
        Conn& cn = conns_[k];
        cn.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (cn.fd < 0) return false;
        int one = 1;
        setsockopt(cn.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const int r = ::connect(cn.fd, reinterpret_cast<sockaddr*>(&addr_), sizeof(addr_));
        if (r < 0 && errno != EINPROGRESS) {
            ::close(cn.fd);
            cn.fd = -1;
            return false;
        }
        epoll_event e{};
        e.events   = EPOLLOUT;
        e.data.u64 = k;
        epoll_ctl(ep_, EPOLL_CTL_ADD, cn.fd, &e);
        return true;
    }

    // 다음 도착 시각(절대 ns)
    int64_t nextArrivalNs() const {
        const double t = std::min(nextPoisson_, syncIdx_ < sync_.size() ? sync_[syncIdx_] : 1e18);
        return t >= 1e17 ? INT64_MAX : t0_ + static_cast<int64_t>(t * 1e9);
    }

    void advanceArrival() {
        const double ts = syncIdx_ < sync_.size() ? sync_[syncIdx_] : 1e18;
        if (nextPoisson_ <= ts) {
            nextPoisson_ += expD_(rng_) / prate_;
        } else {
            ++syncIdx_;
            if (syncIdx_ >= sync_.size()) refillSync();
        }
    }

    // 동기화 몫: 주기마다 f·λ·P 건을 [kP + o, kP + o + w] 에 균등하게 (소수부는 확률로)
    void refillSync() {
        sync_.clear();
        syncIdx_ = 0;
        if (c_.syncFrac <= 0 || c_.syncPeriod <= 0) return;
        const double horizon = c_.warmup + c_.duration;
        while (sync_.empty()) {
            ++syncPeriodIdx_;
            const double start = syncPeriodIdx_ * c_.syncPeriod + c_.syncOffset;
            if (start >= horizon) return;
            const double mean = c_.syncFrac * rate_ * c_.syncPeriod;
            uint64_t     cnt  = static_cast<uint64_t>(mean);
            std::uniform_real_distribution<double> u(0.0, 1.0);
            if (u(rng_) < mean - static_cast<double>(cnt)) ++cnt;
            sync_.resize(cnt);
            for (auto& t : sync_) t = start + u(rng_) * c_.syncWidth;
            std::sort(sync_.begin(), sync_.end());
        }
    }

    void issue(int64_t schedNs, int64_t now) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const uint8_t cls = u(rng_) < c_.complexFrac ? COMPLEX : SIMPLE;
        const double  rel = static_cast<double>(schedNs - t0_) * 1e-9;
        const bool    measured = schedNs >= mStart_ && schedNs < endSend_;
        const int     bin  = std::min<int>(static_cast<int>(rel / kBinSec),
                                           static_cast<int>(res_.bins.size()) - 1);
        const int     fine = std::min<int>(static_cast<int>(rel / kFineSec),
                                           static_cast<int>(res_.fine.size()) - 1);
        ++res_.fine[static_cast<size_t>(fine)];
        ClassStats& st = res_.cls[cls];

        // 빈 칸이 있는 연결을 차례로 찾는다 (최대 64개 확인)
        size_t k = conns_.size();
        for (int tries = 0; tries < 64; ++tries) {
            const size_t cand = cursor_;
            cursor_ = (cursor_ + 1) % conns_.size();
            if (conns_[cand].up && conns_[cand].free > 0) {
                k = cand;
                break;
            }
        }
        if (measured) ++st.sent;
        ++res_.bins[static_cast<size_t>(bin)].sent[cls];
        if (k == conns_.size()) {   // 생성기 쪽 포화: 보내지 못함 (결과에 따로 보고)
            if (measured) ++st.skipped;
            return;
        }

        Conn&  cn = conns_[k];
        Pending* slot = nullptr;
        for (auto& p : cn.pend)
            if (p.id == 0) { slot = &p; break; }
        const uint64_t id = ++nextId_;
        slot->id       = id;
        slot->schedNs  = schedNs;
        slot->cls      = cls;
        slot->measured = measured;
        slot->bin      = bin;
        --cn.free;
        ++outstandingTotal_;

        const int z = static_cast<int>(id % static_cast<uint64_t>(c_.zones));
        char line[160];
        int  len;
        if (cls == COMPLEX) {
            double w = c_.workUs;
            if (c_.workExp) w = expD_(rng_) * c_.workUs;
            const unsigned wu = static_cast<unsigned>(std::max(1.0, std::round(w)));
            len = std::snprintf(line, sizeof(line), "0,%.6f,%.6f,id=%llu,w=%u\n", kZones[z][0],
                                kZones[z][1], static_cast<unsigned long long>(id), wu);
        } else {
            len = std::snprintf(line, sizeof(line), "0,%.6f,%.6f,id=%llu\n", kZones[z][0],
                                kZones[z][1], static_cast<unsigned long long>(id));
        }
        if (measured) res_.genLag.add((now - schedNs) / 1000);

        if (cn.out.empty()) {
            const ssize_t w = ::send(cn.fd, line, static_cast<size_t>(len), MSG_NOSIGNAL);
            if (w == len) return;
            if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                dropConn(k);
                return;
            }
            cn.out.assign(line + std::max<ssize_t>(0, w), static_cast<size_t>(len - std::max<ssize_t>(0, w)));
            epoll_event e{};
            e.events   = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
            e.data.u64 = k;
            epoll_ctl(ep_, EPOLL_CTL_MOD, cn.fd, &e);
        } else {
            cn.out.append(line, static_cast<size_t>(len));
        }
    }

    void flushConn(size_t k) {
        Conn& cn = conns_[k];
        while (!cn.out.empty()) {
            const ssize_t w = ::send(cn.fd, cn.out.data(), cn.out.size(), MSG_NOSIGNAL);
            if (w > 0) {
                cn.out.erase(0, static_cast<size_t>(w));
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            dropConn(k);
            return;
        }
        epoll_event e{};
        e.events   = EPOLLIN | EPOLLRDHUP;
        e.data.u64 = k;
        epoll_ctl(ep_, EPOLL_CTL_MOD, cn.fd, &e);
    }

    void readConn(size_t k) {
        Conn& cn = conns_[k];
        char  buf[8192];
        while (cn.up) {
            const ssize_t r = ::recv(cn.fd, buf, sizeof(buf), 0);
            if (r > 0) {
                cn.in.append(buf, static_cast<size_t>(r));
                size_t pos;
                while ((pos = cn.in.find('\n')) != std::string::npos) {
                    handleLine(cn, cn.in.data(), pos);
                    cn.in.erase(0, pos + 1);
                }
                continue;
            }
            if (r == 0) {
                dropConn(k);
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            dropConn(k);
            return;
        }
    }

    static bool field(const char* p, const char* end, const char* key, uint64_t& out) {
        const size_t klen = std::strlen(key);
        for (const char* s = p; s < end;) {
            const char* bar = static_cast<const char*>(std::memchr(s, '|', static_cast<size_t>(end - s)));
            const char* fe  = bar ? bar : end;
            if (static_cast<size_t>(fe - s) > klen && std::memcmp(s, key, klen) == 0) {
                out = std::strtoull(s + klen, nullptr, 10);
                return true;
            }
            if (!bar) break;
            s = bar + 1;
        }
        return false;
    }

    void handleLine(Conn& cn, const char* p, size_t len) {
        const char* end = p + len;
        uint64_t id = 0;
        if (!field(p, end, "id=", id)) {
            ++res_.unknownId;
            return;
        }
        Pending* slot = nullptr;
        for (auto& pd : cn.pend)
            if (pd.id == id) { slot = &pd; break; }
        if (!slot) {   // 타임아웃 처리한 뒤 늦게 온 응답
            ++res_.unknownId;
            return;
        }
        const int64_t now   = nowNs();
        const int64_t latUs = (now - slot->schedNs) / 1000;
        char tag = 0;
        for (const char* s = p; s + 2 < end; ++s) {
            if (s[0] == 'x' && s[1] == '=' && (s == p || s[-1] == '|')) {
                tag = s[2];
                break;
            }
        }

        Bin& b = res_.bins[static_cast<size_t>(slot->bin)];
        ClassStats& st = res_.cls[slot->cls];
        if (slot->measured) st.lat.add(latUs);
        if (tag == 'S') {
            if (slot->measured) ++st.shed;
            ++b.shed;
        } else if (tag == 'D') {
            if (slot->measured) ++st.late;
            ++b.late;
        } else {
            uint64_t q = 0, s = 0;
            field(p, end, "q=", q);
            field(p, end, "s=", s);
            if (slot->measured) {
                ++st.ok;
                st.latOk.add(latUs);
                st.q.add(static_cast<int64_t>(q));
                st.s.add(static_cast<int64_t>(s));
                st.rest.add(latUs - static_cast<int64_t>(q) - static_cast<int64_t>(s));
                if (static_cast<double>(q) > c_.qThreshMs * 1000.0) ++st.qOver;
            }
            ++b.ok[slot->cls];
            b.latSum[slot->cls] += static_cast<double>(latUs);
            b.latMax[slot->cls] = std::max<uint64_t>(
                    b.latMax[slot->cls], static_cast<uint64_t>(std::max<int64_t>(0, latUs)));
            b.qMax = std::max<uint64_t>(b.qMax, q);
        }
        slot->id = 0;
        ++cn.free;
        --outstandingTotal_;
    }

    void dropConn(size_t k) {
        Conn& cn = conns_[k];
        if (!cn.up) return;
        cn.up = false;
        ++res_.disconnects;
        epoll_ctl(ep_, EPOLL_CTL_DEL, cn.fd, nullptr);
        ::close(cn.fd);
        cn.fd = -1;
        for (auto& pd : cn.pend) {
            if (pd.id == 0) continue;
            if (pd.measured) ++res_.cls[pd.cls].lost;
            pd.id = 0;
            --outstandingTotal_;
        }
        cn.free = c_.outstanding;
        cn.in.clear();
        cn.out.clear();
        startConnect(k);   // 재연결 (서버 유휴 정리 등으로 끊긴 경우) → finishConnect
    }

    void finishConnect(size_t k, uint32_t events) {
        Conn& cn = conns_[k];
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(cn.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0 || (events & (EPOLLERR | EPOLLHUP))) {
            epoll_ctl(ep_, EPOLL_CTL_DEL, cn.fd, nullptr);
            ::close(cn.fd);
            cn.fd = -1;   // 재연결 실패: 이 연결은 더 쓰지 않음
            return;
        }
        cn.up = true;
        epoll_event e{};
        e.events   = EPOLLIN | EPOLLRDHUP;
        e.data.u64 = k;
        epoll_ctl(ep_, EPOLL_CTL_MOD, cn.fd, &e);
    }

    void scanTimeouts(int64_t now) {
        const int64_t limit = static_cast<int64_t>(c_.timeout * 1e9);
        for (auto& cn : conns_) {
            if (cn.free == c_.outstanding) continue;
            for (auto& pd : cn.pend) {
                if (pd.id == 0 || now - pd.schedNs < limit) continue;
                if (pd.measured) ++res_.cls[pd.cls].timeout;
                ++res_.bins[static_cast<size_t>(pd.bin)].timeout;
                pd.id = 0;
                ++cn.free;
                --outstandingTotal_;
            }
        }
    }

    const Config& c_;
    int           id_;
    std::mt19937_64 rng_;
    std::exponential_distribution<double> expD_{1.0};
    std::vector<Conn> conns_;
    sockaddr_in addr_{};
    int     ep_ = -1, tfd_ = -1;
    int64_t t0_ = 0, endSend_ = 0, mStart_ = 0;
    double  rate_ = 0, prate_ = 0, nextPoisson_ = 0;
    std::vector<double> sync_;
    size_t  syncIdx_ = 0;
    int64_t syncPeriodIdx_ = -1;
    size_t  cursor_ = 0;
    uint64_t nextId_ = 0;
    int64_t outstandingTotal_ = 0;
    ThreadResult res_;
};

void raiseFdLimit() {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

double idc(const std::vector<uint64_t>& counts) {   // 분산/평균
    if (counts.empty()) return 0;
    double m = 0;
    for (auto v : counts) m += static_cast<double>(v);
    m /= static_cast<double>(counts.size());
    if (m <= 0) return 0;
    double var = 0;
    for (auto v : counts) var += (static_cast<double>(v) - m) * (static_cast<double>(v) - m);
    var /= static_cast<double>(counts.size());
    return var / m;
}

}  // namespace

int main(int argc, char** argv) {
    Config c;
    if (!parseArgs(argc, argv, c)) return 2;
    raiseFdLimit();

    const double es       = (1 - c.complexFrac) * c.sSimpleUs + c.complexFrac * c.workUs;
    const double peakMean = c.syncFrac > 0 ? c.syncFrac * c.syncPeriod / c.syncWidth + (1 - c.syncFrac) : 1.0;
    std::printf("[loadgen] rate=%.1f/s p_complex=%.5f E[S]=%.1fus rho1=%.3f peak/mean=%.2f "
                "peak_rate=%.0f/s conns=%d threads=%d per_conn_interval=%.1fs\n",
                c.rate, c.complexFrac, es, c.rate * es * 1e-6, peakMean, c.rate * peakMean,
                c.conns, c.threads, c.conns / c.rate);
    if (c.conns / c.rate > 60)
        std::printf("[loadgen] 경고: 연결당 요청 간격이 60초를 넘어 서버 유휴 정리(90초)에 걸릴 수 있음\n");

    std::vector<std::unique_ptr<Worker>> workers;
    for (int t = 0; t < c.threads; ++t) {
        const int from = static_cast<int>(static_cast<int64_t>(c.conns) * t / c.threads);
        const int to   = static_cast<int>(static_cast<int64_t>(c.conns) * (t + 1) / c.threads);
        workers.push_back(std::make_unique<Worker>(c, t, from, to));
    }

    std::atomic<int>     ready{0};
    std::atomic<int64_t> t0{0};
    std::vector<std::thread> threads;
    for (auto& w : workers) {
        threads.emplace_back([&, wp = w.get()] {
            wp->connectAll(ready);
            while (t0.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            wp->run(t0.load());
        });
    }
    while (ready.load() < c.threads) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t connected = 0;
    for (auto& w : workers) connected += w->result().connected;
    std::printf("[loadgen] connected %llu/%d\n", static_cast<unsigned long long>(connected), c.conns);
    if (static_cast<double>(connected) < c.minConnFrac * c.conns) {
        std::printf("[loadgen] 오류: 연결 성공률 %.3f < %.3f — 부하를 걸지 않고 종료\n",
                    static_cast<double>(connected) / c.conns, c.minConnFrac);
        std::fflush(stdout);
        _exit(3);
    }
    const int64_t t0Mono = nowNs() + 500'000'000LL;   // 0.5초 뒤 동시에 시작
    double        t0Epoch;
    {
        timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        const int64_t monoNow = nowNs();
        t0Epoch = static_cast<double>(rt.tv_sec) + static_cast<double>(rt.tv_nsec) * 1e-9
                  + static_cast<double>(t0Mono - monoNow) * 1e-9;
    }
    t0.store(t0Mono);
    for (auto& t : threads) t.join();

    // ── 집계 ──
    ClassStats total[2];
    Hist       genLag;
    std::vector<Bin> bins = workers[0]->result().bins;
    for (auto& b : bins) b = Bin{};
    std::vector<uint64_t> fine(workers[0]->result().fine.size(), 0);
    uint64_t unknown = 0, disc = 0;
    for (auto& w : workers) {
        auto& r = w->result();
        for (int k = 0; k < 2; ++k) total[k].merge(r.cls[k]);
        genLag.merge(r.genLag);
        unknown += r.unknownId;
        disc += r.disconnects;
        for (size_t i = 0; i < bins.size(); ++i) {
            const Bin& s = r.bins[i];
            Bin&       d = bins[i];
            for (int k = 0; k < 2; ++k) {
                d.sent[k] += s.sent[k];
                d.ok[k] += s.ok[k];
                d.latSum[k] += s.latSum[k];
                d.latMax[k] = std::max(d.latMax[k], s.latMax[k]);
            }
            d.shed += s.shed;
            d.late += s.late;
            d.timeout += s.timeout;
            d.qMax = std::max(d.qMax, s.qMax);
        }
        for (size_t i = 0; i < fine.size(); ++i) fine[i] += r.fine[i];
    }
    ClassStats all;
    all.merge(total[0]);
    all.merge(total[1]);

    // 측정 구간의 도착 개수 분산지수 (10ms, 100ms, 1s)
    const size_t f0 = static_cast<size_t>(c.warmup / kFineSec);
    const size_t f1 = std::min(fine.size(), static_cast<size_t>((c.warmup + c.duration) / kFineSec));
    auto agg = [&](size_t width) {
        std::vector<uint64_t> v;
        for (size_t i = f0; i + width <= f1; i += width) {
            uint64_t s = 0;
            for (size_t j = 0; j < width; ++j) s += fine[i + j];
            v.push_back(s);
        }
        return v;
    };
    const double idc10 = idc(agg(1)), idc100 = idc(agg(10)), idc1s = idc(agg(100));

    const char* names[3] = {"SIMPLE", "COMPLEX", "ALL"};
    const ClassStats* rows[3] = {&total[0], &total[1], &all};
    std::printf("\n%-8s %9s %9s %7s %7s %7s %7s %9s %9s %9s %9s %9s %9s\n", "class", "sent", "ok",
                "shed%", "late%", "tmo%", "skip%", "lat_p50", "lat_p99", "lat_p999", "q_p99",
                "q_p999", "s_p50");
    for (int k = 0; k < 3; ++k) {
        const ClassStats& s = *rows[k];
        if (s.sent == 0) continue;
        const double den = static_cast<double>(s.sent);
        std::printf("%-8s %9llu %9llu %6.2f%% %6.2f%% %6.2f%% %6.2f%% %8.2fms %8.2fms %8.2fms "
                    "%8.2fms %8.2fms %7.1fus\n",
                    names[k], static_cast<unsigned long long>(s.sent),
                    static_cast<unsigned long long>(s.ok), 100.0 * s.shed / den,
                    100.0 * s.late / den, 100.0 * s.timeout / den, 100.0 * s.skipped / den,
                    s.lat.pct(0.5) / 1e3, s.lat.pct(0.99) / 1e3, s.lat.pct(0.999) / 1e3,
                    s.q.pct(0.99) / 1e3, s.q.pct(0.999) / 1e3, s.s.pct(0.5));
    }
    std::printf("\ngen_lag p99=%.2fms max=%.2fms  unknown_id=%llu disconnects=%llu lost=%llu  "
                "IDC(10ms/100ms/1s)=%.2f/%.2f/%.2f\n",
                genLag.pct(0.99) / 1e3, static_cast<double>(genLag.max) / 1e3,
                static_cast<unsigned long long>(unknown), static_cast<unsigned long long>(disc),
                static_cast<unsigned long long>(all.lost), idc10, idc100, idc1s);
    for (int k = 0; k < 2; ++k) {
        const ClassStats& s = total[k];
        if (s.ok == 0) continue;
        std::printf("%-8s q>%.0fms=%.3f%%  rest(lat-q-s) p50=%.2fms p99=%.2fms\n", names[k],
                    c.qThreshMs, 100.0 * s.qOver / static_cast<double>(s.ok),
                    s.rest.pct(0.5) / 1e3, s.rest.pct(0.99) / 1e3);
    }
    if (genLag.pct(0.99) > 10'000)
        std::printf("경고: 생성기 송신 지연 p99 > 10ms — 생성기 포화. 결과의 지연에 생성기 몫이 섞임\n");

    if (!c.out.empty()) {
        const std::string sumPath = c.out + "_summary.csv";
        FILE* f = std::fopen(sumPath.c_str(), "w");
        if (f) {
            std::fprintf(f,
                         "label,class,rate,p_complex,work_us,work_dist,sync_frac,sync_width,sync_period,"
                         "conns,sent,ok,shed,late,timeout,skipped,lost,"
                         "lat_p50_ms,lat_p99_ms,lat_p999_ms,lat_max_ms,latok_p99_ms,"
                         "q_p50_ms,q_p99_ms,q_p999_ms,q_max_ms,s_p50_us,s_p99_us,"
                         "gen_lag_p99_ms,idc_10ms,idc_100ms,idc_1s,"
                         "q_thr_ms,q_over,rest_p50_ms,rest_p99_ms,t0_epoch,warmup_s,duration_s,"
                         "connected,disconnects\n");
            for (int k = 0; k < 3; ++k) {
                const ClassStats& s = *rows[k];
                std::fprintf(f,
                             "%s,%s,%.1f,%.6f,%.0f,%s,%.3f,%.2f,%.2f,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                             "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,"
                             "%.1f,%llu,%.3f,%.3f,%.3f,%.1f,%.1f,%llu,%llu\n",
                             c.label.c_str(), names[k], c.rate, c.complexFrac, c.workUs,
                             c.workExp ? "exp" : "fixed", c.syncFrac, c.syncWidth, c.syncPeriod,
                             c.conns, static_cast<unsigned long long>(s.sent),
                             static_cast<unsigned long long>(s.ok),
                             static_cast<unsigned long long>(s.shed),
                             static_cast<unsigned long long>(s.late),
                             static_cast<unsigned long long>(s.timeout),
                             static_cast<unsigned long long>(s.skipped),
                             static_cast<unsigned long long>(s.lost), s.lat.pct(0.5) / 1e3,
                             s.lat.pct(0.99) / 1e3, s.lat.pct(0.999) / 1e3,
                             static_cast<double>(s.lat.max) / 1e3, s.latOk.pct(0.99) / 1e3,
                             s.q.pct(0.5) / 1e3, s.q.pct(0.99) / 1e3, s.q.pct(0.999) / 1e3,
                             static_cast<double>(s.q.max) / 1e3, s.s.pct(0.5), s.s.pct(0.99),
                             genLag.pct(0.99) / 1e3, idc10, idc100, idc1s, c.qThreshMs,
                             static_cast<unsigned long long>(s.qOver), s.rest.pct(0.5) / 1e3,
                             s.rest.pct(0.99) / 1e3, t0Epoch, c.warmup, c.duration,
                             static_cast<unsigned long long>(connected),
                             static_cast<unsigned long long>(disc));
            }
            std::fclose(f);
        }
        const std::string tsPath = c.out + "_ts.csv";
        f = std::fopen(tsPath.c_str(), "w");
        if (f) {
            std::fprintf(f, "t_s,sent_simple,sent_complex,ok_simple,ok_complex,shed,late,timeout,"
                            "lat_mean_simple_ms,lat_max_simple_ms,lat_mean_complex_ms,"
                            "lat_max_complex_ms,q_max_ms\n");
            for (size_t i = 0; i < bins.size(); ++i) {
                const Bin& b   = bins[i];
                const double n0 = static_cast<double>(b.ok[0]);
                const double n1 = static_cast<double>(b.ok[1]);
                std::fprintf(f, "%.1f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                             static_cast<double>(i) * kBinSec,
                             static_cast<unsigned long long>(b.sent[0]),
                             static_cast<unsigned long long>(b.sent[1]),
                             static_cast<unsigned long long>(b.ok[0]),
                             static_cast<unsigned long long>(b.ok[1]),
                             static_cast<unsigned long long>(b.shed),
                             static_cast<unsigned long long>(b.late),
                             static_cast<unsigned long long>(b.timeout),
                             n0 ? b.latSum[0] / n0 / 1e3 : 0.0, static_cast<double>(b.latMax[0]) / 1e3,
                             n1 ? b.latSum[1] / n1 / 1e3 : 0.0, static_cast<double>(b.latMax[1]) / 1e3,
                             static_cast<double>(b.qMax) / 1e3);
            }
            std::fclose(f);
        }
        std::printf("결과: %s, %s\n", sumPath.c_str(), tsPath.c_str());
    }
    return 0;
}
