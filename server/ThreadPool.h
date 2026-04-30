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
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex taskMutex;
    std::condition_variable cv;
    bool stop = false;

public:
    ThreadPool(int numThreads);
    ~ThreadPool();
    
    void enqueue(std::function<void()> task);
    
private:
    void workerFunction();
};

#endif // THREADPOOL_H