// [SHIELD] Public top-level runtime entry point
#pragma once

namespace shield {

/// @brief Runs Shield with the standard CLI, signal handling, and lifecycle.
/// @return Process exit code.
int run(int argc, char** argv);

}  // namespace shield
