// UltraLOB — bounded single-producer/single-consumer ring buffer.
//
// Lock-free with acquire/release ordering and cache-line separated heads. The
// queue never blocks and never drops silently: try_push reports Full and the
// caller must choose a policy. BoundedQueueProducer documents (and counts) the
// three policies the engine supports for feed ingestion:
//
//   Retry    — spin/yield until space is available (default for replay)
//   Block    — same bounded spin, counted separately for callers that want to
//              distinguish "the consumer is behind" from "we chose to wait"
//              (a real condition variable would need shared state the queue
//              deliberately does not own; see docs/design-decisions.md)
//   Drop     — count and discard (explicit, observable, never silent)
//
// A bounded queue cannot guarantee unlimited producer progress when the
// consumer is slower. That is a property of the system, not a bug: the policy
// makes the degradation explicit. backpressure_test.cpp asserts that Retry
// never loses an event and that Drop reports exactly what it discarded.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// std::thread (and std::this_thread::yield) only exist when Emscripten is built
// with pthreads. The browser build is deliberately single-threaded, so the
// producer spin loop degrades to a plain retry loop there.
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#include <thread>
#define ULTRALOB_CAN_YIELD 1
#else
#define ULTRALOB_CAN_YIELD 0
#endif

namespace ultralob {

inline void producer_yield() noexcept {
#if ULTRALOB_CAN_YIELD
  std::this_thread::yield();
#endif
}

enum class ProducerPolicy : std::uint8_t { Retry = 0, Block = 1, Drop = 2 };

inline constexpr std::size_t kCacheLine = 64;

template <typename T, std::size_t Capacity>
class SpscQueue {
 public:
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

  SpscQueue() = default;
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Producer side. False when the queue is full.
  bool try_push(const T& value) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & kMask;
    if (next == tail_.load(std::memory_order_acquire)) {
      full_events_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    buffer_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side. False when the queue is empty.
  bool try_pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    out = buffer_[tail];
    tail_.store((tail + 1) & kMask, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return tail_.load(std::memory_order_acquire) ==
           head_.load(std::memory_order_acquire);
  }

  // Number of items currently queued (approximate under concurrency).
  std::size_t size() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) & kMask;
  }

  static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

  std::uint64_t full_events() const noexcept {
    return full_events_.load(std::memory_order_relaxed);
  }

  void reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    full_events_.store(0, std::memory_order_relaxed);
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // producer writes
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // consumer writes
  alignas(kCacheLine) std::atomic<std::uint64_t> full_events_{0};
  alignas(kCacheLine) std::array<T, Capacity> buffer_{};
};

// Producer-side helper that applies an explicit overflow policy and counts
// exactly what happened. Single producer thread.
template <typename T, std::size_t Capacity>
class BoundedProducer {
 public:
  struct Counters {
    std::uint64_t pushed = 0;
    std::uint64_t retries = 0;
    std::uint64_t dropped = 0;
    std::uint64_t blocked = 0;
  };

  BoundedProducer(SpscQueue<T, Capacity>& queue, ProducerPolicy policy) noexcept
      : queue_(&queue), policy_(policy) {}

  // Returns false only when the policy dropped the item.
  bool push(const T& value) noexcept {
    while (!queue_->try_push(value)) {
      switch (policy_) {
        case ProducerPolicy::Retry:
          counters_.retries++;
          producer_yield();
          break;
        case ProducerPolicy::Block:
          // Documented as "blocking" but implemented as counted spin+yield:
          // the queue owns no mutex/condition variable, so claiming a real
          // blocking wait here would be false.
          counters_.blocked++;
          producer_yield();
          break;
        case ProducerPolicy::Drop:
          counters_.dropped++;
          return false;
      }
    }
    counters_.pushed++;
    return true;
  }

  const Counters& counters() const noexcept { return counters_; }
  ProducerPolicy policy() const noexcept { return policy_; }

 private:
  SpscQueue<T, Capacity>* queue_;
  ProducerPolicy policy_;
  Counters counters_{};
};

}  // namespace ultralob
