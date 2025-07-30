#include "shield/commands/all_commands.hpp"
#include <csignal>

// Global flag for signal handling
volatile sig_atomic_t g_signal_status = 0;

// Global signal handler
void signal_handler(int signal) {
    g_signal_status = signal;
}