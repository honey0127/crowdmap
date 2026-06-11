#include "ThreadPool.h"

//생성자 : numThreads(=4)개 워커를 미리 생성
ThreadPool::ThreadPool(int numThreads, size_t maxQueueSize)
        : maxQueue(maxQueueSize) {
    for (int i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] { workerFunction(); });
    }
}
//클라이언트가 100명 붙어도 스레드는 4개만 존재.
// 스레드 생성/ 소멸 비용(수백 마이크로 초)을 없애는게 핵심
ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(taskMutex);
        stop = true;
    }
    cv.notify_all();
    for (std::thread& w : workers) {
        w.join();
    }
}

bool ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(taskMutex); // 락 걸고
        // 포화 상태에서 계속 받으면 큐가 무한히 자라 지연이 폭발한다.
        // 받지 않고 false를 돌려줘 호출자가 즉시 대체 응답(load shedding)하게 한다.
        if (tasks.size() >= maxQueue) return false;
        tasks.push(std::move(task)); // 큐에 추가 (복사 대신 move)
    }
    cv.notify_one(); // 자고 있는 워커 하나만 깨움(notify_all 이 아닌 이유 : 불필요한 wake-up 방지
    return true;
}

size_t ThreadPool::pending() {
    std::unique_lock<std::mutex> lock(taskMutex);
    return tasks.size();
}

void ThreadPool::workerFunction() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(taskMutex);
            cv.wait(lock, [this] { return !tasks.empty() || stop; }); // 할 일 없으면 CPU 0%에서 대기

            if (stop && tasks.empty()) break; // 종료 조건
            if (tasks.empty()) continue;

            task = std::move(tasks.front()); // 큐에서 꺼내고
            tasks.pop(); // 락 헤재 후 task 실행 ( 락 밖에서 실행해야 병렬 처리됨 )
        }
        task();
    }
}
// cv.wait의 람다 조건이 매우 중요. 아무 이유 없이 깨어나는 현상을 막아준다.
// 아무 이유 없이 깨어나면 왜 안되는가? race condition?
