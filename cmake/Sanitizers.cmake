# TSan and ASan are mutually exclusive; CI runs them as separate jobs.
# Never measure performance under a sanitizer: TSan costs 5-15x time, 5-10x memory.
add_library(etg_sanitizers INTERFACE)

if(ETG_SANITIZER STREQUAL "asan")
  target_compile_options(etg_sanitizers INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(etg_sanitizers    INTERFACE -fsanitize=address,undefined)
elseif(ETG_SANITIZER STREQUAL "tsan")
  target_compile_options(etg_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(etg_sanitizers    INTERFACE -fsanitize=thread)
elseif(ETG_SANITIZER STREQUAL "ubsan")
  target_compile_options(etg_sanitizers INTERFACE -fsanitize=undefined -fno-sanitize-recover=all)
  target_link_options(etg_sanitizers    INTERFACE -fsanitize=undefined)
elseif(NOT ETG_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "ETG_SANITIZER must be one of: none asan tsan ubsan (got '${ETG_SANITIZER}')")
endif()
