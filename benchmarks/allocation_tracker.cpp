#include "allocation_tracker.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_deallocations{0};
std::atomic<std::uint64_t> g_bytes{0};

void* tracked_allocate(std::size_t size, std::size_t alignment) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(size, std::memory_order_relaxed);
  if (alignment > 0) {
    const std::size_t rounded = ((size + alignment - 1) / alignment) * alignment;
    void* pointer = std::aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
    if (pointer == nullptr) throw std::bad_alloc();
    return pointer;
  }
  void* pointer = std::malloc(size == 0 ? 1 : size);
  if (pointer == nullptr) throw std::bad_alloc();
  return pointer;
}
}  // namespace

void* operator new(std::size_t size) { return tracked_allocate(size, 0); }
void* operator new[](std::size_t size) { return tracked_allocate(size, 0); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return tracked_allocate(size, 0);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return tracked_allocate(size, 0);
  } catch (...) {
    return nullptr;
  }
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  return tracked_allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return tracked_allocate(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer) noexcept {
  if (pointer == nullptr) return;
  g_deallocations.fetch_add(1, std::memory_order_relaxed);
  std::free(pointer);
}
void operator delete[](void* pointer) noexcept { operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { operator delete(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { operator delete(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { operator delete(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { operator delete(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  operator delete(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  operator delete(pointer);
}

namespace ultralob_bench {

AllocationSnapshot allocations() noexcept {
  AllocationSnapshot snapshot{};
  snapshot.allocations = g_allocations.load(std::memory_order_relaxed);
  snapshot.deallocations = g_deallocations.load(std::memory_order_relaxed);
  snapshot.bytes = g_bytes.load(std::memory_order_relaxed);
  return snapshot;
}

void reset_allocations() noexcept {
  g_allocations.store(0, std::memory_order_relaxed);
  g_deallocations.store(0, std::memory_order_relaxed);
  g_bytes.store(0, std::memory_order_relaxed);
}

}  // namespace ultralob_bench
