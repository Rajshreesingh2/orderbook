#pragma once
// Single-producer / single-consumer queues, in three flavors, so you can SEE what matters:
//
//   MutexQueue       std::mutex around a ring buffer. Simple, correct, slow under contention.
//   SpscQueueNaive   lock-free, but head and tail sit on the same cache line (false sharing)
//                    and every access is seq_cst.
//   SpscQueue        lock-free + cache-line padding + acquire/release only + cached
//                    copies of the other side's index (so most pushes/pops touch NO shared line).
//
// Contract for the lock-free ones: exactly ONE thread calls try_push, exactly ONE calls try_pop.
// Capacity must be a power of two. T must be trivially copyable (we copy it into the ring).
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

constexpr std::size_t kCacheLine = 64;

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// Pin the calling thread to one CPU core (Linux only; returns false if it fails).
inline bool pin_this_thread(int core) {
#ifdef __linux__
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core; return false;
#endif
}

// ---------------------------------------------------------------------------------------
template <class T, std::size_t Cap>
class SpscQueue {
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap must be a power of two");
public:
    bool try_push(const T& v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);   // only we write tail_
        if (t - head_cache_ == Cap) {                                   // looks full: refresh
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == Cap) return false;
        }
        buf_[t & (Cap - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);                  // publish the slot
        return true;
    }
    bool try_pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);   // only we write head_
        if (h == tail_cache_) {                                         // looks empty: refresh
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = buf_[h & (Cap - 1)];
        head_.store(h + 1, std::memory_order_release);                  // give the slot back
        return true;
    }
private:
    // Indices only grow; slot = index & (Cap-1); full when tail - head == Cap.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};   // producer's line:
    std::size_t head_cache_{0};                              //   tail_ + its cached view of head_
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};   // consumer's line:
    std::size_t tail_cache_{0};                              //   head_ + its cached view of tail_
    alignas(kCacheLine) std::array<T, Cap> buf_{};
};

// ---------------------------------------------------------------------------------------
template <class T, std::size_t Cap>
class SpscQueueNaive {
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap must be a power of two");
public:
    bool try_push(const T& v) {
        const std::size_t t = tail_.load();                  // seq_cst (the default)
        if (t - head_.load() == Cap) return false;           // reads the other side every time
        buf_[t & (Cap - 1)] = v;
        tail_.store(t + 1);
        return true;
    }
    bool try_pop(T& out) {
        const std::size_t h = head_.load();
        if (h == tail_.load()) return false;
        out = buf_[h & (Cap - 1)];
        head_.store(h + 1);
        return true;
    }
private:
    std::atomic<std::size_t> head_{0};   // head_ and tail_ share one cache line:
    std::atomic<std::size_t> tail_{0};   // every push invalidates the consumer's line and vice versa
    std::array<T, Cap> buf_{};
};

// ---------------------------------------------------------------------------------------
template <class T, std::size_t Cap>
class MutexQueue {
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap must be a power of two");
public:
    bool try_push(const T& v) {
        std::lock_guard<std::mutex> g(m_);
        if (size_ == Cap) return false;
        buf_[(head_ + size_) & (Cap - 1)] = v;
        ++size_;
        return true;
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> g(m_);
        if (size_ == 0) return false;
        out = buf_[head_];
        head_ = (head_ + 1) & (Cap - 1);
        --size_;
        return true;
    }
private:
    std::mutex m_;
    std::size_t head_ = 0, size_ = 0;
    std::array<T, Cap> buf_{};
};

// ---------------------------------------------------------------------------------------
// Spin until the operation succeeds. Yields now and then so the code still makes progress
// when there are fewer CPU cores than spinning threads.
template <class Q, class T>
inline void push_blocking(Q& q, const T& v) {
    unsigned spins = 0;
    while (!q.try_push(v)) { cpu_relax(); if ((++spins & 1023u) == 0) std::this_thread::yield(); }
}
template <class Q, class T>
inline void pop_blocking(Q& q, T& out) {
    unsigned spins = 0;
    while (!q.try_pop(out)) { cpu_relax(); if ((++spins & 1023u) == 0) std::this_thread::yield(); }
}
