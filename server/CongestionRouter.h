#pragma once
#include "ExternalCongestionClient.h"
#include "CongestionCalculator.h"
#include "CacheManager.h"
#include "Logger.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <string>

class CongestionRouter {
public:
    // Sync : 조회 경로에서 외부 API 를 직접 호출한다 (v4 방식, 실험 D 의 기준).
    // Async: 조회는 캐시/옛 캐시/내부 계산으로 즉시 응답하고, 만료된 구역의 외부 API
    //        갱신은 REFRESH 큐 → 전용 I/O 스레드 1개가 맡는다.
    //        v4 워커 1 의 지연 튐(API 를 기다리는 동안 워커 처리율 0)을 조회 경로에서 없앤다.
    enum class Mode { Sync, Async };

    ~CongestionRouter() { stop(); }

    // 외부 API 클라이언트들을 우선순위 순서로 등록 (start 전에만)
    void addClient(std::shared_ptr<ExternalCongestionClient> client) {
        clients.push_back(client);
    }

    // Async 모드면 I/O 스레드를 시작한다. 호출한 스레드의 CPU 고정을 물려받는다.
    void start(Mode m, CacheManager& cache) {
        mode = m;
        if (mode != Mode::Async || ioRunning.exchange(true)) return;
        ioCache  = &cache;
        ioThread = std::thread([this] { ioLoop(); });
    }

    void stop() {
        if (!ioRunning.exchange(false)) return;
        refreshCv.notify_all();
        if (ioThread.joinable()) ioThread.join();
    }

    Mode currentMode() const { return mode; }

    // lat/lng → 혼잡도 결과
    CongestionResult resolve(double lat, double lng,
                             int internalUserCount,
                             CacheManager& cache,
                             int zoneId) {
        return (mode == Mode::Async)
               ? resolveAsync(lat, lng, internalUserCount, cache, zoneId)
               : resolveSync(lat, lng, internalUserCount, cache, zoneId);
    }

    // 운영 지표: 갱신 대기 구역 수
    size_t refreshPending() {
        std::lock_guard<std::mutex> lock(inflightMutex);
        return refreshQueue.size();
    }

private:
    // ── Async: 캐시 → (만료면 갱신 예약) → 옛 캐시 → 내부 계산 ─────────────
    CongestionResult resolveAsync(double lat, double lng, int internalUserCount,
                                  CacheManager& cache, int zoneId) {
        CongestionResult cached;
        if (cache.get(zoneId, cached)) return cached;

        submitRefresh(zoneId, lat, lng, internalUserCount);

        if (cache.getStale(zoneId, cached, STALE_MAX_AGE_SEC)) return cached;
        // 처음 보는 구역: 갱신이 끝나기 전까지는 내부 계산으로 응답(캐싱하지 않음)
        return internalCalc.calculateCongestion(internalUserCount);
    }

    struct RefreshTask {
        int    zoneId;
        double lat, lng;
        int    internalUserCount;
    };

    // 구역 중복 제거: 같은 구역의 갱신이 이미 대기·진행 중이면 넣지 않는다.
    void submitRefresh(int zoneId, double lat, double lng, int internalUserCount) {
        {
            std::lock_guard<std::mutex> lock(inflightMutex);
            if (refreshQueue.size() >= REFRESH_QUEUE_MAX) return;   // 다음 조회 때 다시 시도
            if (!inflight.insert(zoneId).second) return;
            refreshQueue.push_back({zoneId, lat, lng, internalUserCount});
        }
        refreshCv.notify_one();
    }

    void ioLoop() {
        while (true) {
            RefreshTask t;
            {
                std::unique_lock<std::mutex> lock(inflightMutex);
                refreshCv.wait(lock, [this] { return !refreshQueue.empty() || !ioRunning; });
                if (!ioRunning) return;
                t = refreshQueue.front();
                refreshQueue.pop_front();
            }
            fetchAndStore(t.lat, t.lng, t.internalUserCount, *ioCache, t.zoneId);
            std::lock_guard<std::mutex> lock(inflightMutex);
            inflight.erase(t.zoneId);
        }
    }

    // ── Sync (v4 와 같음) ──────────────────────────────────────────────
    // 우선순위: 캐시 → 외부 API → 내부 계산 fallback
    //
    // [캐시 스탬피드 방지] 인기 존의 캐시가 만료되는 순간 수천 조회가 동시에
    // miss를 내면, 워커들이 같은 외부 API를 한꺼번에 두드린다(stampede).
    //  - single-flight: 존당 한 스레드만 외부 API 갱신을 수행
    //  - stale-while-revalidate: 갱신 중인 다른 스레드는 만료된 값을 즉시 응답
    //  - negative cache: 실패(fallback) 결과는 짧은 TTL(10초)로만 캐싱
    CongestionResult resolveSync(double lat, double lng,
                                 int internalUserCount,
                                 CacheManager& cache,
                                 int zoneId) {
        // 1. 캐시 먼저 확인
        CongestionResult cached;
        if (cache.get(zoneId, cached)) {
            Log::debug("[Router] Cache hit zone=" + std::to_string(zoneId));
            return cached;
        }

        // 2. single-flight 선점: 이 존의 외부 API 갱신 권한을 한 스레드만 가진다
        bool claimed;
        {
            std::lock_guard<std::mutex> lock(inflightMutex);
            claimed = inflight.insert(zoneId).second;
        }
        if (!claimed) {
            // 다른 워커가 갱신 중: 낡은 값이라도 즉시 응답 (외부 API 중복 호출 차단)
            if (cache.getStale(zoneId, cached, STALE_MAX_AGE_SEC)) {
                Log::debug("[Router] Stale hit zone=" + std::to_string(zoneId));
                return cached;
            }
            // stale조차 없으면 내부 계산으로 즉시 응답. 갱신 중인 스레드가
            // 곧 캐시를 채울 것이므로 이 결과는 캐싱하지 않는다.
            return internalCalc.calculateCongestion(internalUserCount);
        }

        // 선점 성공: 어떤 경로로 빠져나가든 inflight 표식을 반드시 해제
        struct InflightGuard {
            CongestionRouter* router;
            int zone;
            ~InflightGuard() {
                std::lock_guard<std::mutex> lock(router->inflightMutex);
                router->inflight.erase(zone);
            }
        } guard{this, zoneId};

        return fetchAndStore(lat, lng, internalUserCount, cache, zoneId);
    }

    // 3·4단계: 외부 API → 실패 시 내부 계산(negative cache). Sync 워커와 Async I/O 스레드 공용.
    CongestionResult fetchAndStore(double lat, double lng, int internalUserCount,
                                   CacheManager& cache, int zoneId) {
        // double-check: 선점 직전에 다른 스레드가 갱신을 끝냈을 수 있다
        CongestionResult cached;
        if (cache.get(zoneId, cached)) return cached;

        // 외부 API 순서대로 시도
        for (auto& client : clients) {
            if (!client->covers(lat, lng)) continue;

            auto ext = client->getCongestion(lat, lng);
            if (!ext.valid) continue;

            Log::debug("[Router] External hit source=" + ext.source);
            CongestionResult r = externalToResult(ext);
            cache.set(zoneId, r);
            return r;
        }

        // Fallback: 내부 사용자 수 기반 계산.
        // 기본 TTL(30초)로 캐싱하면 외부 API 복구가 30초씩 가려지고,
        // 캐싱하지 않으면 다음 조회가 곧바로 또 외부 API를 두드린다.
        // → 짧은 TTL의 negative cache가 둘 사이의 균형점.
        Log::debug("[Router] Fallback internal count=" + std::to_string(internalUserCount));
        CongestionResult r = internalCalc.calculateCongestion(internalUserCount);
        cache.set(zoneId, r, FAILURE_TTL_SEC);
        return r;
    }

    static constexpr int    STALE_MAX_AGE_SEC = 300;  // stale 응답 허용 최대 나이
    static constexpr int    FAILURE_TTL_SEC   = 10;   // 실패 결과의 짧은 TTL
    // 갱신 대기 구역 상한 = 캐시 최대 크기(main: 2000). 캐시에 담을 수 없는 구역까지
    // 갱신해 봐야 LRU 로 곧 밀려나므로 의미가 없다.
    static constexpr size_t REFRESH_QUEUE_MAX = 2000;

    std::vector<std::shared_ptr<ExternalCongestionClient>> clients;
    CongestionCalculator internalCalc;
    Mode mode = Mode::Sync;

    std::mutex inflightMutex;
    std::unordered_set<int> inflight; // 외부 API 갱신이 대기·진행 중인 zoneId 집합
    std::deque<RefreshTask> refreshQueue;
    std::condition_variable refreshCv;
    std::atomic<bool>       ioRunning{false};
    std::thread             ioThread;
    CacheManager*           ioCache = nullptr;

    CongestionResult externalToResult(const ExternalCongestionResult& ext) {
        CongestionLevel lvl;
        switch (ext.level) {
            case 1:  lvl = CongestionLevel::RELAXED;  break;
            case 2:  lvl = CongestionLevel::MODERATE; break;
            default: lvl = CongestionLevel::CROWDED;  break;
        }
        return {lvl, ext.level / 4.0, 0};
    }
};
