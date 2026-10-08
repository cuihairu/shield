// [SHIELD] Crash collector (Crashpad) glue — see docs/crash-reporting.md
#pragma once

namespace shield::crash {

// Initializes the Crashpad client from the `crash.*` config keys: locates
// the crashpad_handler executable, prepares the dump directory, surfaces
// dumps left over from a previous run as a WARNING log, installs the
// version annotation, and starts the out-of-process handler. Called from
// bootstrap::initialize_impl right after config files and the logger are
// up. Never throws and never blocks startup: any failure degrades to an
// ERROR log ("crash collector degraded") and the runtime keeps running
// without crash capture. Returns true when the handler was armed.
bool initialize();

// Deliberate null-pointer dereference behind `shield --crash-test`. Kept
// as a distinct no-inline function so it appears as its own frame in the
// symbolized stack (that frame is the acceptance assertion). Not linked
// into builds with SHIELD_ENABLE_CRASHPAD off.
[[noreturn]] void crash_test_null_deref();

}  // namespace shield::crash
