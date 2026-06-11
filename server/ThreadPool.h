#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <cstddef>

class ThreadPool {
private:
    std::vector<std::thread> workers; // 미리 만들어준 고정 스레드들
    std::queue<std::function<void()>> tasks; // 처리 대기 중인 작업 큐
    std::mutex taskMutex; // 큐에 동시 접근할 때 충돌 방지
    std::condition_variable cv; // 일 없을 때 CPU 낭비 없이 대기
    bool stop = false; // 서버 종료 신호
    size_t maxQueue; // 큐 길이 상한 (무한 적체 → 메모리 폭주/지연 폭발 방지)

public:
    // maxQueueSize: 큐가 이 길이에 도달하면 enqueue가 false를 반환한다.
    // 호출자(리액터)는 이를 신호로 load shedding(stale 응답/폐기)을 수행한다.
    explicit ThreadPool(int numThreads, size_t maxQueueSize = 10'000);
    ~ThreadPool();

    // 큐가 포화면 작업을 받지 않고 false 반환 (백프레셔 신호)
    bool enqueue(std::function<void()> task);

    // 현재 대기 중인 작업 수 (운영 지표용)
    size_t pending();

private:
    void workerFunction();
};

#endif // THREADPOOL_H
