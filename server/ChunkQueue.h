#pragma once
#include <cstddef>
#include <utility>

/**
 * @brief 블록을 이어 붙이는 FIFO 큐 (스레드 안전하지 않음 — 호출자가 잠근다)
 *
 *  - N칸짜리 블록을 필요할 때 tail 에 이어 붙이고, head 블록이 비면 떼어 낸다.
 *    기존 내용을 복사하지 않으므로 버스트 순간에 재할당으로 멈추지 않는다.
 *  - 떼어 낸 빈 블록은 여분(spare) 1개까지만 보관하고 나머지는 반납한다.
 *    블록 경계에서 할당·반납이 반복되는 것을 막기 위해서다.
 *  - 큐가 완전히 비면 여분도 반납해, 한가할 때는 블록 1개만 남는다.
 *
 * 블록 크기 N = 1024: 최대 유입 초당 75k 에서 할당이 초당 약 73회.
 * (libstdc++ std::deque 는 512B 단위라 64B 칸이면 8칸마다 할당 → 초당 약 9,400회)
 */
template <typename T, size_t N = 1024>
class ChunkQueue {
public:
    static constexpr size_t kChunkSlots = N;

    ChunkQueue() = default;
    ChunkQueue(const ChunkQueue&) = delete;
    ChunkQueue& operator=(const ChunkQueue&) = delete;

    ~ChunkQueue() {
        while (head_) {
            Chunk* next = head_->next;
            delete head_;
            head_ = next;
        }
        delete spare_;
    }

    void push(T&& v) {
        if (!tail_) {
            head_ = tail_ = grab();
            headIdx_ = tailIdx_ = 0;
        } else if (tailIdx_ == N) {
            Chunk* c = grab();
            tail_->next = c;
            tail_ = c;
            tailIdx_ = 0;
        }
        tail_->slot[tailIdx_++] = std::move(v);
        ++size_;
    }

    bool pop(T& out) {
        if (size_ == 0) return false;
        out = std::move(head_->slot[headIdx_++]);
        --size_;

        if (size_ == 0) {
            // 빈 큐: 남은 블록(head == tail)을 처음부터 재사용하고 여분은 반납
            headIdx_ = tailIdx_ = 0;
            if (spare_) {
                delete spare_;
                spare_ = nullptr;
                --chunks_;
            }
        } else if (headIdx_ == N) {
            Chunk* old = head_;
            head_ = head_->next;
            headIdx_ = 0;
            release(old);
        }
        return true;
    }

    size_t size()   const { return size_; }
    bool   empty()  const { return size_ == 0; }
    size_t chunks() const { return chunks_; }   // 보유 중인 블록 수(여분 포함)

private:
    struct Chunk {
        T      slot[N];
        Chunk* next = nullptr;
    };

    Chunk* grab() {
        if (spare_) {
            Chunk* c = spare_;
            spare_ = nullptr;
            c->next = nullptr;
            return c;
        }
        ++chunks_;
        return new Chunk();
    }

    void release(Chunk* c) {
        if (!spare_) {
            c->next = nullptr;
            spare_ = c;
        } else {
            delete c;
            --chunks_;
        }
    }

    Chunk* head_  = nullptr;
    Chunk* tail_  = nullptr;
    Chunk* spare_ = nullptr;
    size_t headIdx_ = 0;
    size_t tailIdx_ = 0;
    size_t size_    = 0;
    size_t chunks_  = 0;
};
