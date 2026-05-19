#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace nano
{
/**
 * Dumps an async-signal-safe binary stacktrace file. The companion
 * dump_crash_stacktrace_readable() should be called right after to also write a
 * human-readable version (the binary dump alone cannot be symbolicated from a
 * different process once ASLR has relocated the binary).
 */
void dump_crash_stacktrace ();

/**
 * Writes a human-readable, already-symbolicated stacktrace next to the binary
 * dump. Not async-signal-safe (allocates), so it must be called only as a
 * best-effort step *after* dump_crash_stacktrace() has already succeeded, while
 * still in the address space of the crashing process.
 */
void dump_crash_stacktrace_readable ();

/**
 * Generates the current stacktrace
 */
std::string generate_stacktrace ();

/**
 * Sets the directory that the crash dumps are written into. Must be called once
 * during single-threaded startup, before the abort signal handler can fire.
 * When unset, the dumps are written to the current working directory (legacy
 * behaviour). Used so crash dumps land on the persistent data volume rather than
 * an ephemeral container working directory.
 */
void set_crash_stacktrace_path (std::filesystem::path const & directory);

/**
 * Async-signal-safe accessor for the directory configured via
 * set_crash_stacktrace_path(). Returns an empty string when unset (legacy
 * current-working-directory behaviour). Used by the load-address dump writer so
 * those files land next to the binary dump on the persistent data volume.
 */
char const * crash_stacktrace_directory ();

/**
 * Scans known locations (the current working directory and the data path) for
 * crash stacktrace dumps and writes them to `out`, preferring the human-readable
 * version and falling back to decoding the binary dump. Returns the number of
 * crashes printed.
 *
 * @param include_archived also report previously archived dumps, not just the active one
 * @param archive_after rename each active dump aside after printing so it is reported exactly once
 * @param prefer_advanced_decode reconstruct from the binary dump via the advanced addr2line
 *        ceremony instead of the crash-time readable text; the readable text is only used as a
 *        fallback when no binary dump exists for a crash
 */
std::size_t output_stacktrace_dumps (std::filesystem::path const & data_path, std::ostream & out, bool include_archived, bool archive_after, bool prefer_advanced_decode);
}
