#include <catch2/catch_test_macros.hpp>

#include "version.hpp"

// Step 1 exists to prove the build, link and test plumbing works end to end.
// It is replaced by the record layout tests in step 2.
TEST_CASE("version is non-empty and matches the project version", "[skeleton]") {
  REQUIRE_FALSE(etg::version().empty());
  REQUIRE(etg::version() == "0.1.0");
}
