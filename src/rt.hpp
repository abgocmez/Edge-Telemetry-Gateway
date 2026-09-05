#pragma once

#include <string>

namespace etg::rt {

// Lock the process into RAM with mlockall(MCL_CURRENT | MCL_FUTURE).
//
// Priority decides who runs when several threads are runnable. It says nothing
// about a thread that is not runnable because the page it touched is not
// resident: that thread waits for the kernel to fetch it, and SCHED_FIFO does
// not shorten the wait. Measuring this pipeline under contention showed exactly
// that shape - real-time priority recovered p50, p90 and p99 and left p99.9 and
// max where they were - so the flag exists to test whether residency is what
// the remaining tail is made of.
//
// Needs privilege or a raised RLIMIT_MEMLOCK, and is therefore opt-in: a daemon
// that silently fails to lock its memory is worse than one that never tried,
// because it reports the same numbers either way. On failure this returns false
// with the reason and the caller decides.
[[nodiscard]] bool lock_memory(std::string& error);

}  // namespace etg::rt
