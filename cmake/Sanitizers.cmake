# TSan and ASan are mutually exclusive; CI runs them as separate jobs.
# Never measure performance under a sanitizer: TSan costs 5-15x time, 5-10x memory.
add_library(etg_sanitizers INTERFACE)

if(ETG_SANITIZER STREQUAL "asan")
  target_compile_options(etg_sanitizers INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(etg_sanitizers    INTERFACE -fsanitize=address,undefined)
elseif(ETG_SANITIZER STREQUAL "tsan")
  # -Wno-tsan disables one specific GCC diagnostic: that std::atomic_thread_fence
  # is not modelled by ThreadSanitizer. The seqlock reader in broadcast_ring.hpp
  # needs an acquire fence, and it is the standard formulation - the same one the
  # Linux kernel's seqlock uses.
  #
  # Suppressing it costs nothing real. TSan finds *missing* synchronisation; it
  # never validates that the synchronisation present is sufficient, which is a
  # limitation this project already relies on knowing. The ring's payload is held
  # in relaxed atomics precisely so TSan can still see every access, so its actual
  # job is unaffected; what it cannot reason about is an ordering question it was
  # never able to answer. That question is settled on real aarch64 hardware
  # instead, which is why the Pi is a required part of the plan rather than a
  # nicer place to run the same tests.
  target_compile_options(etg_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer -Wno-tsan)
  target_link_options(etg_sanitizers    INTERFACE -fsanitize=thread)
elseif(ETG_SANITIZER STREQUAL "ubsan")
  target_compile_options(etg_sanitizers INTERFACE -fsanitize=undefined -fno-sanitize-recover=all)
  target_link_options(etg_sanitizers    INTERFACE -fsanitize=undefined)
elseif(NOT ETG_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "ETG_SANITIZER must be one of: none asan tsan ubsan (got '${ETG_SANITIZER}')")
endif()
