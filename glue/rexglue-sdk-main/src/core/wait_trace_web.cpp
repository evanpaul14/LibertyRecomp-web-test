/**
 * @file        core/wait_trace_web.cpp
 * @brief       Per-thread blocking-point records for web hang reports.
 */

#include <rex/thread/wait_trace.h>

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include <emscripten/emscripten.h>

#include <rex/logging.h>

namespace rex::thread::wait_trace {
namespace {

constexpr int kMaximumDepth = 4;

struct Level {
  std::atomic<double> start_ms{0.0};
  char text[192] = {};
};

// One per thread; never freed, so Dump() can read it without coordination.
// The owning thread writes its levels; Dump() reads them racily, which is
// acceptable for a diagnostic.
struct Slot {
  int tid = 0;
  char name[32] = {};
  std::atomic<int> depth{0};
  std::atomic<bool> exited{false};
  Level levels[kMaximumDepth];
};

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<uintptr_t, Slot*>& Registry() {
  static auto* registry = new std::unordered_map<uintptr_t, Slot*>();
  return *registry;
}

std::array<std::atomic<uint64_t>, size_t(Counter::kCount)> counters{};

Slot* SlotFor(uintptr_t pthread) {
  std::lock_guard lock(RegistryMutex());
  auto& slot = Registry()[pthread];
  if (!slot) slot = new Slot();
  return slot;
}

struct CurrentSlot {
  Slot* slot = nullptr;
  ~CurrentSlot() {
    if (slot) slot->exited.store(true, std::memory_order_relaxed);
  }
};
thread_local CurrentSlot current_slot;

Slot* Current() {
  if (!current_slot.slot) {
    current_slot.slot = SlotFor(uintptr_t(pthread_self()));
    current_slot.slot->tid = gettid();
  }
  return current_slot.slot;
}

}  // namespace

Scope::Scope(const char* format, ...) {
  Slot* slot = Current();
  const int depth = slot->depth.load(std::memory_order_relaxed);
  recorded_ = depth < kMaximumDepth;
  if (recorded_) {
    Level& level = slot->levels[depth];
    va_list args;
    va_start(args, format);
    std::vsnprintf(level.text, sizeof(level.text), format, args);
    va_end(args);
    level.start_ms.store(emscripten_get_now(), std::memory_order_relaxed);
  }
  slot->depth.store(depth + 1, std::memory_order_release);
}

Scope::~Scope() {
  Slot* slot = current_slot.slot;
  slot->depth.store(slot->depth.load(std::memory_order_relaxed) - 1, std::memory_order_release);
}

void SetThreadName(uintptr_t pthread, const char* name) {
  Slot* slot = SlotFor(pthread);
  std::lock_guard lock(RegistryMutex());
  std::snprintf(slot->name, sizeof(slot->name), "%s", name);
}

void Increment(Counter counter) {
  counters[size_t(counter)].fetch_add(1, std::memory_order_relaxed);
}

void Dump(const char* reason) {
  const double now = emscripten_get_now();
  REXLOG_WARN("wait trace ({}): suspends sent={} delivered={} resumed={}", reason,
              counters[size_t(Counter::kSuspendSent)].load(),
              counters[size_t(Counter::kSuspendDelivered)].load(),
              counters[size_t(Counter::kSuspendWaitDone)].load());
  std::lock_guard lock(RegistryMutex());
  for (const auto& [pthread, slot] : Registry()) {
    if (slot->exited.load(std::memory_order_relaxed)) continue;
    const int depth = std::min(slot->depth.load(std::memory_order_acquire), kMaximumDepth);
    std::string line;
    for (int i = 0; i < depth; ++i) {
      const Level& level = slot->levels[i];
      char part[256];
      std::snprintf(part, sizeof(part), "%s[%.0f ms] %s", i ? " > " : "",
                    now - level.start_ms.load(std::memory_order_relaxed), level.text);
      line += part;
    }
    REXLOG_WARN("  tid {} '{}': {}", slot->tid, slot->name,
                depth ? line : std::string("running (or blocked outside a traced wait)"));
  }
}

}  // namespace rex::thread::wait_trace
