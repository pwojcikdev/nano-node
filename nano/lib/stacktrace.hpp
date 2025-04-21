#pragma once

#include <string>

namespace nano
{
/**
 * Installs a signal handler for SIGSEGV and SIGABRT signals
 * This is used to generate a stacktrace when the process crashes
 * Should be called at the very start of the program
 */
void install_abort_signal_handler ();

/**
 * Dumps a stacktrace file which can be read using the --debug_output_last_backtrace_dump CLI command
 */
void dump_crash_stacktrace ();

/**
 * Loads the last crash stacktrace from the dump file
 */
std::string load_last_crash_stacktrace (std::chrono::seconds max_age = std::chrono::days{ 2 });

/**
 * Generates the current stacktrace
 */
std::string generate_stacktrace ();
}