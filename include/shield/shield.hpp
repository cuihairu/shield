// [SHIELD] Public top-level runtime entry point
#pragma once

namespace shield {

/// @brief Runs Shield with the standard CLI, signal handling, and lifecycle.
/// @return Process exit code.
int run(int argc, char** argv);

/// @brief Requests a cooperative shutdown of a running shield::run() loop.
///
/// Signal handlers installed by shield::run() call this internally; it is
/// also the supported way for embedders and tests to stop the runtime
/// without a real OS signal (raise(SIGINT) cannot reach the Windows
/// console handler, so cross-platform tests should use this entry point).
void request_stop();

}  // namespace shield
