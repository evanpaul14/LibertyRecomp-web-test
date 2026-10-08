/**
 * @file        thread/wait_trace.h
 * @brief       Records what each thread is blocked on, for hang reports.
 *
 * Web builds only (elsewhere every call compiles to nothing). A Web Worker's
 * stack cannot be sampled from outside, so blocking points record a short
 * description while they wait and Dump() lists every thread's current one.
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>

namespace rex::thread::wait_trace {

#if REX_PLATFORM_WEB

// Describes what the calling thread waits on until the scope ends. Scopes
// nest (up to a small fixed depth); Dump() prints the whole stack.
class Scope {
 public:
  explicit Scope(const char* format, ...) __attribute__((format(printf, 2, 3)));
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  bool recorded_;
};

// Names a thread in the report (called wherever threads get named).
void SetThreadName(uintptr_t pthread, const char* name);

// Counts an event shown at the top of the report (fixed set of counters).
enum class Counter { kSuspendSent, kSuspendDelivered, kSuspendWaitDone, kCount };
void Increment(Counter counter);

// Logs every known thread and what it is waiting on, as warnings.
void Dump(const char* reason);

#else

class Scope {
 public:
  explicit Scope(const char*, ...) {}
};
inline void SetThreadName(uintptr_t, const char*) {}
enum class Counter { kSuspendSent, kSuspendDelivered, kSuspendWaitDone, kCount };
inline void Increment(Counter) {}
inline void Dump(const char*) {}

#endif

}  // namespace rex::thread::wait_trace
