#include "ThreadPool.h"

//생성자 : numThreads(=4)개 워커를 미리 생성
ThreadPool::ThreadPool(int numThreads) {
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

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(taskMutex); // 락 걸고
        tasks.push(task); // 큐에 추가
    }
    cv.notify_one(); // 자고 있는 워커 하나만 깨움(notify_all 이 아닌 이유 : 불필요한 wake-up 방지
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