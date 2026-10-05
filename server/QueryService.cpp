#include "QueryService.h"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int STALE_MAX_AGE_SEC = 300;
constexpr int SHED_INTERVAL_HINT = 30;   // 혼잡 상황이므로 리포트 주기를 늦추라는 힌트

int64_t threadCpuNs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

}  // namespace

void QueryService::burnCpuUs(uint32_t us) {
    if (us == 0) return;
    const int64_t until = threadCpuNs() + static_cast<int64_t>(us) * 1000;
    uint64_t x = 0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>(until);
    do {
        for (int i = 0; i < 2000; ++i) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            asm volatile("" : "+r"(x));   // 최적화로 루프가 사라지지 않게
        }
    } while (threadCpuNs() < until);
}

void QueryService::sendAll(int fd, const char* data, size_t len) {
    // 응답이 짧아(수십 바이트) 보통 한 번에 끝나지만, 송신 버퍼가 가득 찬 느린
    // 클라이언트에서는 잠시(최대 300ms) 기다렸다 재시도하고, 그래도 안 되면 폐기한다
    // — 느린 소비자가 워커를 오래 붙잡으면 그 자체가 새로운 병목이 되기 때문이다.
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

int QueryService::intervalHintFor(CongestionLevel level) {
    switch (level) {
        case CongestionLevel::CROWDED:  return 30;
        case CongestionLevel::MODERATE: return 15;
        default:                        return 10;
    }
}

void QueryService::process(Job& job, int64_t deqNs) {
#ifdef CROWDMAP_EXPERIMENT
    burnCpuUs(std::min(job.workUs, MAX_WORK_US));
#endif

    std::string response;
    if (job.zoneId == -1) {
        response = "RELAXED|0.0|interval=10";
    } else {
        size_t localDensity = engine_.getDensity(job.lat, job.lon);
        int zoneReport = engine_.getZoneReport(job.zoneId);
        size_t effectiveDensity = (zoneReport > 0)
            ? std::max(localDensity, static_cast<size_t>(zoneReport))
            : localDensity;

        // 캐시 확인/갱신 예약(Async) 또는 single-flight(Sync)는 router 가 일원화해 처리
        CongestionResult result = router_.resolve(job.lat, job.lon,
                                                  static_cast<int>(effectiveDensity),
                                                  cache_, job.zoneId);

        // interval 힌트: 혼잡할수록 클라이언트 리포트 주기를 늦춰
        // 서버 유입량을 원격으로 줄인다 (클라이언트는 자기 위치 조회
        // 응답의 힌트만 배치 주기에 반영한다)
        response = std::string(result.levelString()) + "|"
                   + std::to_string(result.ratio)
                   + "|interval=" + std::to_string(intervalHintFor(result.level));
    }

#ifdef CROWDMAP_EXPERIMENT
    if (job.reqId) response += "|id=" + std::to_string(job.reqId);
    const int64_t doneNs = monoNowNs();
    response += "|q=" + std::to_string((deqNs - job.tEnqNs) / 1000)
                + "|s=" + std::to_string((doneNs - deqNs) / 1000);
#else
    (void)deqNs;
#endif
    response += "\n";

    sendAll(job.fd, response.data(), response.size());
    ::close(job.fd);
    job.fd = -1;
}

std::string QueryService::staleLine(int zoneId, uint64_t reqId, char tag, int64_t waitUs) {
    CongestionResult stale;
    std::string line;
    if (zoneId != -1 && cache_.getStale(zoneId, stale, STALE_MAX_AGE_SEC)) {
        line = std::string(stale.levelString()) + "|" + std::to_string(stale.ratio)
               + "|interval=" + std::to_string(SHED_INTERVAL_HINT);
    }
#ifdef CROWDMAP_EXPERIMENT
    // 부하 생성기가 shed·마감 초과를 타임아웃과 구분해 셀 수 있도록 항상 응답한다
    if (line.empty()) line = "NONE|0|interval=" + std::to_string(SHED_INTERVAL_HINT);
    if (reqId) line += "|id=" + std::to_string(reqId);
    if (waitUs >= 0) line += "|q=" + std::to_string(waitUs);
    line += "|x=";
    line += tag;
#else
    (void)reqId;
    (void)tag;
    (void)waitUs;
#endif
    if (!line.empty()) line += "\n";
    return line;
}

void QueryService::expire(Job& job) {
    const std::string line =
            staleLine(job.zoneId, job.reqId, 'D', (monoNowNs() - job.tEnqNs) / 1000);
    if (!line.empty()) sendAll(job.fd, line.data(), line.size());
    ::close(job.fd);
    job.fd = -1;
}

void QueryService::shedReply(int fd, int zoneId, uint64_t reqId) {
    const std::string line = staleLine(zoneId, reqId, 'S');
    // 리액터 스레드: 논블로킹 1회 시도만. 안 나가면 폐기.
    if (!line.empty()) ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
}
