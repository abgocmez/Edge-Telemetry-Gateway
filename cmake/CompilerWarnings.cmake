# Applied via the etg_warnings INTERFACE target rather than globally, so
# FetchContent dependencies are not held to our warning level.
add_library(etg_warnings INTERFACE)

target_compile_options(etg_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wnon-virtual-dtor
  -Wold-style-cast      # wire parsing must be explicit about conversions
  -Wcast-align
  -Wconversion          # integer width juggling in the record layout
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
)

if(ETG_WERROR)
  target_compile_options(etg_warnings INTERFACE -Werror)
endif()
