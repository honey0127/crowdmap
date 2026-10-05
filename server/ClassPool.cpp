#include "ClassPool.h"
#include "CpuTopology.h"
#include "Logger.h"

#include <algorithm>
#include <ctime>
#include <pthread.h>
#include <unistd.h>

int64_t monoNowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

ClassPool::ClassPool(Options opt, ProcessFn process, ExpireFn expire)
        : opt_(std::move(opt)), process_(std::move(process)), expire_(std::move(expire)) {
    const int n = std::max(1, opt_.threads);
    active_.store(std::clamp(opt_.initialActive, 1, n));
    cap_.store(opt_.initialCap);
    stats_ = std::make_unique<WorkerStats[]>(static_cast<size_t>(n));
    workers_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this, i] { workerLoop(i); });
        clockid_t cid;
        if (pthread_getcpuclockid(workers_.back().native_handle(), &cid) == 0)
            cpuClocks_.push_back(cid);
    }
}

double ClassPool::cpuSeconds() const {
    double sum = 0.0;
    for (clockid_t cid : cpuClocks_) {
        timespec ts;
        if (clock_gettime(cid, &ts) == 0)
            sum += static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    }
    return sum;
}

ClassPool::~ClassPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    workCv_.notify_all();
    parkCv_.notify_all();
    for (auto& t : workers_) t.join();

    // 종료 시 남은 작업의 소켓 정리
    Job j;
    while (q_.pop(j)) {
        if (j.fd >= 0) ::close(j.fd);
    }
}

bool ClassPool::tryEnqueue(Job& job) {
    {
        std::lock_guard<std::mutex> lk(m_);
        ++offered_;
        if (q_.size() >= cap_.load(std::memory_order_relaxed)) {
            ++shed_;
            return false;
        }
        job.tEnqNs = monoNowNs();
        q_.push(std::move(job));
        qlen_.store(q_.size(), std::memory_order_relaxed);
    }
    workCv_.notify_one();   // 깨어 있는 스레드 중 하나만 (재워 둔 스레드는 parkCv_ 에서 대기)
    return true;
}

void ClassPool::setActive(int n) {
    n = std::clamp(n, 1, threads());
    {
        std::lock_guard<std::mutex> lk(m_);
        if (active_.load(std::memory_order_relaxed) == n) return;
        active_.store(n, std::memory_order_relaxed);
    }
    // 늘릴 때: 재워 둔 스레드를 깨운다. 줄일 때: 일을 기다리던 스레드가 재우기 대상인지 다시 본다.
    parkCv_.notify_all();
    workCv_.notify_all();
}

void ClassPool::setCap(size_t k) {
    cap_.store(std::max<size_t>(1, k), std::memory_order_relaxed);
}

PoolSnapshot ClassPool::snapshot() {
    PoolSnapshot s;
    {
        std::lock_guard<std::mutex> lk(m_);
        s.offered  = offered_;
        s.shed     = shed_;
        s.dequeued = dequeued_;
        s.qlen     = q_.size();
        s.chunks   = q_.chunks();
    }
    s.expired = expired_.load(std::memory_order_relaxed);
    for (int i = 0; i < threads(); ++i) {
        const auto& w = stats_[static_cast<size_t>(i)];
        s.done     += w.done.load(std::memory_order_relaxed);
        s.svcSum   += w.svc.load(std::memory_order_relaxed);
        s.svcSqSum += w.svcSq.load(std::memory_order_relaxed);
        s.waitSum  += w.wait.load(std::memory_order_relaxed);
    }
    s.active = active();
    s.cap    = cap();
    return s;
}

void ClassPool::workerLoop(int idx) {
    if (!opt_.cpus.empty() && !cpu::pinThisThread(opt_.cpus)) {
        Log::warn("[ClassPool] " + opt_.name + " 코어 고정 실패 idx=" + std::to_string(idx));
    }
    if (opt_.nice != 0) cpu::setThreadNice(opt_.nice);

    WorkerStats& st = stats_[static_cast<size_t>(idx)];
    std::unique_lock<std::mutex> lk(m_);
    while (true) {
        if (stop_) break;

        if (idx >= active_.load(std::memory_order_relaxed)) {
            parkCv_.wait(lk, [&] {
                return stop_ || idx < active_.load(std::memory_order_relaxed);
            });
            continue;
        }
        if (q_.empty()) {
            workCv_.wait(lk, [&] {
                return stop_ || !q_.empty() || idx >= active_.load(std::memory_order_relaxed);
            });
            continue;
        }

        Job job;
        q_.pop(job);
        ++dequeued_;
        qlen_.store(q_.size(), std::memory_order_relaxed);
        lk.unlock();

        const int64_t deqNs = monoNowNs();
        if (opt_.deadlineNs > 0 && deqNs - job.tEnqNs > opt_.deadlineNs) {
            expired_.fetch_add(1, std::memory_order_relaxed);
            expire_(job);
        } else {
            process_(job, deqNs);
            const double svc  = static_cast<double>(monoNowNs() - deqNs) * 1e-9;
            const double wait = static_cast<double>(deqNs - job.tEnqNs) * 1e-9;
            // 쓰는 스레드가 하나뿐이라 load+store 로 충분 (읽는 쪽은 컨트롤러)
            st.svc.store(st.svc.load(std::memory_order_relaxed) + svc, std::memory_order_relaxed);
            st.svcSq.store(st.svcSq.load(std::memory_order_relaxed) + svc * svc,
                           std::memory_order_relaxed);
            st.wait.store(st.wait.load(std::memory_order_relaxed) + wait,
                          std::memory_order_relaxed);
            st.done.store(st.done.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        }
        lk.lock();
    }
}
