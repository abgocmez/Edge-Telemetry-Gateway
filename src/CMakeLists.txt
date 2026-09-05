add_library(etg_core STATIC
  version.cpp
  wire.cpp
  clock.cpp
  can_source.cpp
  synthetic_source.cpp
  samples.cpp
  fanout.cpp
  tcp.cpp
  egress.cpp
  frame_stream.cpp
)
target_include_directories(etg_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(etg_core PRIVATE etg_warnings PUBLIC etg_sanitizers Threads::Threads)
target_compile_definitions(etg_core PRIVATE ETG_VERSION="${PROJECT_VERSION}")

add_executable(etg-gateway main.cpp)
target_link_libraries(etg-gateway PRIVATE etg_core etg_warnings)

add_executable(etg-gen gen_main.cpp)
target_link_libraries(etg-gen PRIVATE etg_core etg_warnings)

add_executable(etg-probe probe_main.cpp)
target_link_libraries(etg-probe PRIVATE etg_core etg_warnings)

add_executable(etg-record record_main.cpp)
target_link_libraries(etg-record PRIVATE etg_core etg_warnings)
