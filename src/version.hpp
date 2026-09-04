#pragma once

#include <string_view>

namespace etg {

// Project version, injected from CMake. Exists so the step-1 skeleton has a real
// symbol to link and assert against rather than an empty library.
std::string_view version() noexcept;

}  // namespace etg
