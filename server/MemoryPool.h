#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <vector>
#include <mutex>
#include <iostream>
#include <memory>
#include <stack>

/**
 * @brief Zero-copy 수신을 위한 고정 크기 메모리 풀
 * 
 * L5 계층에서 데이터 수신 시 OS 커널 버퍼에서 유저 공간으로의 복사 횟수를 최소화하고,
 * 빈번한 new/delete에 의한 Heap 단편화 및 할당 지연을 방지하기 위해 설계되었습니다.
 */
class MemoryPool {
public:
    static constexpr size_t CHUNK_SIZE = 8192; // 8KB (MTU 고려 및 대용량 패킷 대응)
    
    explicit MemoryPool(size_t initial_chunks = 1024) {
        expand(initial_chunks);
    }

    ~MemoryPool() {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        for (void* ptr : all_allocations_) {
            free(ptr);
        }
    }

    /**
     * @brief 풀에서 빈 메모리 블록을 획득합니다. (O(1))
     */
    void* allocate() {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (free_stack_.empty()) {
            expand(all_allocations_.size()); // 필요 시 2배 확장
        }
        void* ptr = free_stack_.top();
        free_stack_.pop();
        return ptr;
    }

    /**
     * @brief 사용이 끝난 메모리 블록을 풀에 반납합니다. (O(1))
     */
    void deallocate(void* ptr) {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(pool_mutex_);
        free_stack_.push(ptr);
    }

    /**
     * @brief 병목 최소화 전략:
     * 현재는 중앙 Mutex를 사용하지만, 극단적인 성능이 필요한 경우 
     * thread_local 풀을 구성하거나 Lock-free Stack을 고려할 수 있습니다.
     */

private:
    void expand(size_t num_chunks) {
        // 정렬된 메모리 할당 (SIMD 최적화 대비)
        void* block = nullptr;
        if (posix_memalign(&block, 64, num_chunks * CHUNK_SIZE) != 0) {
            throw std::bad_alloc();
        }
        
        all_allocations_.push_back(block);
        for (size_t i = 0; i < num_chunks; ++i) {
            free_stack_.push(static_cast<char*>(block) + (i * CHUNK_SIZE));
        }
    }

    std::mutex pool_mutex_;
    std::stack<void*> free_stack_;
    std::vector<void*> all_allocations_; // 전체 할당된 블록 추적 (해제용)
};

#endif // MEMORY_POOL_H
