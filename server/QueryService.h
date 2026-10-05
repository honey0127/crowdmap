#ifndef QUERY_SERVICE_H
#define QUERY_SERVICE_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "CacheManager.h"
#include "ClassPool.h"
#include "CongestionRouter.h"
#include "SpatialDensityEngine.h"

/**
 * @brief 조회 1건의 처리와 응답 (워커 스레드), 대체 응답 (shed: 리액터 / 마감 초과: 워커)
 *
 * 응답: "LEVEL|ratio|interval=N" (+ 실험 빌드 확장 필드) "\n"
 *  - 실험 빌드(CROWDMAP_EXPERIMENT)에서만 붙는 필드
 *      |id=<요청 번호>   요청에 id= 가 있을 때. 한 연결에 여러 요청이 동시에 걸려 있고
 *                        SIMPLE/COMPLEX 큐가 따로라 응답 순서가 요청 순서와 다를 수 있다.
 *      |q=<µs>|s=<µs>    큐 대기(꺼냄 − 투입), 처리(송신 직전 − 꺼냄)
 *      |x=S              상한 초과로 받지 않음(shed) → 옛 캐시 응답
 *      |x=D              꺼냈을 때 마감(DEADLINE_MS) 초과 → 옛 캐시 응답 (+|q=<µs> 기다린 시간)
 *    앱(ServerClient.parseResponse)은 모르는 필드를 무시하므로 호환된다.
 */
class QueryService {
public:
    QueryService(SpatialDensityEngine& engine, CongestionRouter& router, CacheManager& cache)
        : engine_(engine), router_(router), cache_(cache) {}

    // 워커: 처리 + 응답 + fd close
    void process(Job& job, int64_t deqNs);
    // 워커: 마감 초과 → 옛 캐시 응답 + fd close
    void expire(Job& job);
    // 리액터: 상한 초과 → 옛 캐시로 1회 논블로킹 송신 (fd 는 연결 소켓 그대로, close 안 함)
    void shedReply(int fd, int zoneId, uint64_t reqId);

    // 부분 전송(EAGAIN/short write)을 처리하며 응답을 보낸다.
    static void sendAll(int fd, const char* data, size_t len);

    // 혼잡 레벨 → 클라이언트 권장 리포트 주기(초).
    // 밀집 존일수록 개별 리포트의 정보 가치가 낮으므로(존 리포트가 대체)
    // 주기를 늦춰 서버 유입량을 원격으로 감압한다.
    static int intervalHintFor(CongestionLevel level);

    // 실험 빌드: COMPLEX 합성 작업. 스레드 CPU 시간으로 us 만큼 연산한다.
    // 벽시계로 재면 코어를 뺏긴 동안에도 작업이 진행된 것으로 쳐서 경합을 숨기므로 CPU 시간을 쓴다.
    static void burnCpuUs(uint32_t us);

    static constexpr uint32_t MAX_WORK_US = 100'000;   // COMPLEX 설계값 10ms 의 10배

private:
    // 옛 캐시(최대 300초) 응답 문자열. 없으면 빈 문자열 (실험 빌드는 NONE 으로 항상 응답)
    // waitUs ≥ 0 이면 실험 빌드에서 |q=<µs> 를 붙인다(마감 초과: 그만큼 기다렸다는 기록)
    std::string staleLine(int zoneId, uint64_t reqId, char tag, int64_t waitUs = -1);

    SpatialDensityEngine& engine_;
    CongestionRouter&     router_;
    CacheManager&         cache_;
};

#endif // QUERY_SERVICE_H
