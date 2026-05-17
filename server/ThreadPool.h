#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>

class ThreadPool {
private:
    std::vector<std::thread> workers; // 미리 만들어준 고정 스레드들
    std::queue<std::function<void()>> tasks; // 처리 대기 중인 작업 큐
    std::mutex taskMutex; // 큐에 동시 접근할 때 충돌 방지
    std::condition_variable cv; // 일 없을 때 CPU 낭비 없이 대기
    bool stop = false; // 서버 종료 신호

public:
    ThreadPool(int numThreads);
    ~ThreadPool();
    
    void enqueue(std::function<void()> task);
    
private:
    void workerFunction();
};

#endif // THREADPOOL_H