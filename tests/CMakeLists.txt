include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(etg_tests
  test_version.cpp
  test_wire.cpp
  test_source.cpp
  test_samples.cpp
  test_bounded_queue.cpp
  test_fanout.cpp
  test_egress.cpp
)
target_link_libraries(etg_tests PRIVATE etg_core etg_warnings Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(etg_tests)
