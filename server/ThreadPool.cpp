#include "ThreadPool.h"

ThreadPool::ThreadPool(int numThreads) {
    for (int i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] { workerFunction(); });
    }
}

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
        std::unique_lock<std::mutex> lock(taskMutex);
        tasks.push(task);
    }
    cv.notify_one();
}

void ThreadPool::workerFunction() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(taskMutex);
            cv.wait(lock, [this] { return !tasks.empty() || stop; });
            
            if (stop && tasks.empty()) break;
            if (tasks.empty()) continue;
            
            task = std::move(tasks.front());
            tasks.pop();
        }
        task();
    }
}