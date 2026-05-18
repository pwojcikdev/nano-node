#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace nano
{
/**
 * Dumps a stacktrace file which can be read using the --debug_output_last_backtrace_dump CLI command
 */
void dump_crash_stacktrace ();

/**
 * Generates the current stacktrace
 */
std::string generate_stacktrace ();

/**
 * Sets the directory that dump_crash_stacktrace() writes the crash dump into.
 * Must be called once during single-threaded startup, before the abort signal
 * handler can fire. When unset, the dump is written to the current working
 * directory (legacy behaviour). Used so crash dumps land on the persistent data
 * volume rather than an ephemeral container working directory.
 */
void set_crash_stacktrace_path (std::filesystem::path const & directory);

/**
 * Scans known locations (the current working directory and the data path) for
 * crash stacktrace dump files written by dump_crash_stacktrace(), decodes them
 * and writes the resulting stacktraces to `out`. Returns the number of dumps
 * printed.
 *
 * @param include_archived also report previously archived dumps, not just the active one
 * @param archive_after rename each active dump aside after printing so it is reported exactly once
 */
std::size_t output_stacktrace_dumps (std::filesystem::path const & data_path, std::ostream & out, bool include_archived, bool archive_after);
}
