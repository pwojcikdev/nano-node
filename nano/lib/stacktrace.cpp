#include <nano/boost/stacktrace.hpp>
#include <nano/lib/stacktrace.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <vector>

namespace
{
constexpr char const * backtrace_binary_filename = "nano_node_backtrace.dump";
constexpr char const * backtrace_readable_filename = "nano_node_backtrace.txt";

// Async-signal-safe storage for the crash dump paths. These are fixed buffers
// written once during single-threaded startup and only ever read (never
// reallocated) from the abort signal handler.
constexpr std::size_t crash_path_max = 4096;
char crash_binary_path[crash_path_max] = "nano_node_backtrace.dump";
char crash_readable_path[crash_path_max] = "nano_node_backtrace.txt";

void store_path (char (&buffer)[crash_path_max], std::filesystem::path const & path)
{
	auto str = path.string ();
	if (str.size () < crash_path_max)
	{
		std::memcpy (buffer, str.c_str (), str.size () + 1);
	}
}

std::vector<std::filesystem::path> scan_directories (std::filesystem::path const & data_path)
{
	std::vector<std::filesystem::path> directories;
	std::error_code ec;
	auto cwd = std::filesystem::current_path (ec);
	directories.push_back (ec ? std::filesystem::path (".") : cwd);
	directories.push_back (data_path);
	return directories;
}

bool all_digits (std::string const & value)
{
	return !value.empty () && std::all_of (value.begin (), value.end (), [] (unsigned char c) { return std::isdigit (c) != 0; });
}

// A crash produces a readable (.txt) and a binary (.dump) file sharing a suffix:
// "" for the active crash, ".<unix-timestamp>" once archived.
struct crash_group
{
	std::filesystem::path readable;
	std::filesystem::path binary;
};
}

void nano::dump_crash_stacktrace ()
{
	boost::stacktrace::safe_dump_to (crash_binary_path);
}

void nano::dump_crash_stacktrace_readable ()
{
	// Best-effort. Runs after the async-safe binary dump, in the crashing
	// process, so the stacktrace is symbolicated against the correct (current)
	// load addresses. Not async-signal-safe, but we are aborting anyway and the
	// binary dump has already succeeded, so a failure here loses nothing.
	std::ofstream ofs (crash_readable_path, std::ios::out | std::ios::trunc);
	if (ofs.is_open ())
	{
		ofs << boost::stacktrace::stacktrace ();
	}
}

std::string nano::generate_stacktrace ()
{
	auto stacktrace = boost::stacktrace::stacktrace ();
	std::stringstream ss;
	ss << stacktrace;
	return ss.str ();
}

void nano::set_crash_stacktrace_path (std::filesystem::path const & directory)
{
	store_path (crash_binary_path, directory / backtrace_binary_filename);
	store_path (crash_readable_path, directory / backtrace_readable_filename);
}

std::size_t nano::output_stacktrace_dumps (std::filesystem::path const & data_path, std::ostream & out, bool include_archived, bool archive_after)
{
	std::string const binary_name = backtrace_binary_filename;
	std::string const readable_name = backtrace_readable_filename;
	std::string const binary_prefix = binary_name + ".";
	std::string const readable_prefix = readable_name + ".";

	// Group files by suffix so the readable and binary halves of the same crash are handled together
	std::map<std::string, crash_group> groups;
	std::set<std::filesystem::path> seen;
	for (auto const & directory : scan_directories (data_path))
	{
		std::error_code ec;
		if (!std::filesystem::is_directory (directory, ec))
		{
			continue;
		}
		for (auto const & entry : std::filesystem::directory_iterator (directory, ec))
		{
			if (ec)
			{
				break;
			}
			if (!entry.is_regular_file (ec))
			{
				continue;
			}
			auto name = entry.path ().filename ().string ();

			bool readable;
			std::string suffix;
			if (name == readable_name)
			{
				readable = true;
			}
			else if (name == binary_name)
			{
				readable = false;
			}
			else if (name.rfind (readable_prefix, 0) == 0 && all_digits (name.substr (readable_prefix.size ())))
			{
				readable = true;
				suffix = name.substr (readable_name.size ());
			}
			else if (name.rfind (binary_prefix, 0) == 0 && all_digits (name.substr (binary_prefix.size ())))
			{
				readable = false;
				suffix = name.substr (binary_name.size ());
			}
			else
			{
				continue;
			}

			if (!suffix.empty () && !include_archived)
			{
				continue;
			}

			std::error_code cec;
			auto canonical = std::filesystem::weakly_canonical (entry.path (), cec);
			if (!seen.insert (cec ? entry.path () : canonical).second)
			{
				continue;
			}

			auto & group = groups[suffix];
			auto & slot = readable ? group.readable : group.binary;
			if (slot.empty ())
			{
				slot = entry.path ();
			}
		}
	}

	std::size_t printed = 0;
	for (auto const & [suffix, group] : groups)
	{
		bool emitted = false;

		// Prefer the human-readable, already-symbolicated stacktrace
		if (!group.readable.empty ())
		{
			std::ifstream ifs (group.readable);
			std::stringstream contents;
			contents << ifs.rdbuf ();
			if (!contents.str ().empty ())
			{
				out << "==== Crash stacktrace dump: " << group.readable.string () << " ====\n"
					<< contents.str ();
				if (contents.str ().back () != '\n')
				{
					out << '\n';
				}
				out << "==== End of crash stacktrace dump ====" << std::endl;
				emitted = true;
			}
		}

		// Fall back to decoding the binary dump (raw addresses if produced by another process)
		if (!emitted && !group.binary.empty ())
		{
			std::ifstream ifs (group.binary, std::ios::binary);
			if (ifs.is_open ())
			{
				boost::stacktrace::stacktrace st = boost::stacktrace::stacktrace::from_dump (ifs);
				if (!st.empty ())
				{
					out << "==== Crash stacktrace dump: " << group.binary.string () << " ====\n"
						<< st
						<< "==== End of crash stacktrace dump ====" << std::endl;
					emitted = true;
				}
			}
		}

		if (emitted)
		{
			++printed;
		}

		if (archive_after && suffix.empty ())
		{
			auto timestamp = std::to_string (std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now ().time_since_epoch ()).count ());
			for (auto const & active : { group.readable, group.binary })
			{
				if (!active.empty ())
				{
					std::filesystem::path archived = active;
					archived += "." + timestamp;
					std::error_code rec;
					std::filesystem::rename (active, archived, rec);
				}
			}
		}
	}
	return printed;
}
