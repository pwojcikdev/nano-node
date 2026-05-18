#include <nano/boost/stacktrace.hpp>
#include <nano/lib/stacktrace.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <ostream>
#include <set>
#include <sstream>
#include <vector>

namespace
{
constexpr char const * backtrace_dump_filename = "nano_node_backtrace.dump";

// Async-signal-safe storage for the crash dump path. It is a fixed buffer
// written once during single-threaded startup and only ever read (never
// reallocated) from the abort signal handler.
constexpr std::size_t crash_dump_path_max = 4096;
char crash_dump_path[crash_dump_path_max] = "nano_node_backtrace.dump";

std::vector<std::filesystem::path> scan_directories (std::filesystem::path const & data_path)
{
	std::vector<std::filesystem::path> directories;
	std::error_code ec;
	auto cwd = std::filesystem::current_path (ec);
	directories.push_back (ec ? std::filesystem::path (".") : cwd);
	directories.push_back (data_path);
	return directories;
}
}

void nano::dump_crash_stacktrace ()
{
	boost::stacktrace::safe_dump_to (crash_dump_path);
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
	auto full = (directory / backtrace_dump_filename).string ();
	if (full.size () < crash_dump_path_max)
	{
		std::memcpy (crash_dump_path, full.c_str (), full.size () + 1);
	}
}

std::size_t nano::output_stacktrace_dumps (std::filesystem::path const & data_path, std::ostream & out, bool include_archived, bool archive_after)
{
	auto const active_name = std::string (backtrace_dump_filename);
	auto const archived_prefix = active_name + ".";

	// Collect candidate dump files across known locations, de-duplicated by canonical path
	std::vector<std::filesystem::path> dumps;
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
			bool active = (name == active_name);
			bool archived = include_archived && name.rfind (archived_prefix, 0) == 0;
			if (!active && !archived)
			{
				continue;
			}
			std::error_code cec;
			auto canonical = std::filesystem::weakly_canonical (entry.path (), cec);
			if (seen.insert (cec ? entry.path () : canonical).second)
			{
				dumps.push_back (entry.path ());
			}
		}
	}

	std::sort (dumps.begin (), dumps.end ());

	std::size_t printed = 0;
	for (auto const & dump : dumps)
	{
		std::ifstream ifs (dump, std::ios::binary);
		if (!ifs.is_open ())
		{
			continue;
		}
		boost::stacktrace::stacktrace st = boost::stacktrace::stacktrace::from_dump (ifs);
		ifs.close ();
		if (st.empty ())
		{
			continue;
		}

		out << "==== Crash stacktrace dump: " << dump.string () << " ====\n"
			<< st
			<< "==== End of crash stacktrace dump ====" << std::endl;
		++printed;

		if (archive_after && dump.filename ().string () == active_name)
		{
			auto timestamp = std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now ().time_since_epoch ()).count ();
			std::filesystem::path archived = dump;
			archived += "." + std::to_string (timestamp);
			std::error_code rec;
			std::filesystem::rename (dump, archived, rec);
		}
	}
	return printed;
}
