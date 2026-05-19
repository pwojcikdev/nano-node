#include <nano/boost/stacktrace.hpp>
#include <nano/lib/stacktrace.hpp>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#include <unwind.h>
#endif

namespace
{
constexpr char const * backtrace_binary_filename = "nano_node_backtrace.dump";
constexpr char const * backtrace_readable_filename = "nano_node_backtrace.txt";
constexpr char const * load_address_prefix = "nano_node_crash_load_address_dump_";

// Async-signal-safe storage for the crash dump paths. These are fixed buffers
// written once during single-threaded startup and only ever read (never
// reallocated) from the abort signal handler.
constexpr std::size_t crash_path_max = 4096;
char crash_binary_path[crash_path_max] = "nano_node_backtrace.dump";
char crash_readable_path[crash_path_max] = "nano_node_backtrace.txt";
char crash_directory[crash_path_max] = "";

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

// Parse a whole token as a hexadecimal number. The entire token must be
// consumed, so frame indices like "0#" are rejected while "0x000064A2..." is
// accepted.
bool parse_hex (std::string const & token, std::uint64_t & out)
{
	if (token.empty ())
	{
		return false;
	}
	std::size_t pos = 0;
	try
	{
		out = std::stoull (token, &pos, 16);
	}
	catch (...)
	{
		return false;
	}
	return pos == token.size ();
}

std::string shell_single_quote (std::string const & value)
{
	std::string result = "'";
	for (char c : value)
	{
		if (c == '\'')
		{
			result += "'\\''";
		}
		else
		{
			result += c;
		}
	}
	result += "'";
	return result;
}

#if defined(__linux__)
// Best-effort reconstruction of a symbolicated stacktrace from a binary dump
// produced by a previous (now exited) process, using the recorded load-address
// files and addr2line. This is the "advanced ceremony" that copes with ASLR;
// returns false if it cannot be done (no load-address files, addr2line missing,
// nothing resolvable) so the caller can fall back to raw addresses.
bool symbolicate_binary_dump (std::filesystem::path const & dump_path, std::ostream & out)
{
	std::ifstream ifs (dump_path, std::ios::binary);
	if (!ifs.is_open ())
	{
		return false;
	}
	boost::stacktrace::stacktrace st = boost::stacktrace::stacktrace::from_dump (ifs);
	ifs.close ();
	if (st.empty ())
	{
		return false;
	}

	// The load-address files are written next to the dump (legacy runs put them in the cwd)
	std::vector<std::filesystem::path> candidate_dirs;
	candidate_dirs.push_back (dump_path.parent_path ().empty () ? std::filesystem::path (".") : dump_path.parent_path ());
	std::error_code cwd_ec;
	if (auto cwd = std::filesystem::current_path (cwd_ec); !cwd_ec)
	{
		candidate_dirs.push_back (cwd);
	}

	std::filesystem::path base_dir;
	for (auto const & dir : candidate_dirs)
	{
		std::error_code ec;
		if (std::filesystem::exists (dir / (std::string (load_address_prefix) + "0.txt"), ec))
		{
			base_dir = dir;
			break;
		}
	}
	if (base_dir.empty ())
	{
		return false;
	}

	boost::system::error_code dll_ec;
	auto executable = boost::dll::program_location (dll_ec);
	if (dll_ec)
	{
		return false;
	}

	struct base_entry
	{
		std::uint64_t address;
		std::string library;
	};
	std::vector<base_entry> bases;

	// File 0 holds a single line: the load address of the executable itself
	{
		std::ifstream file (base_dir / (std::string (load_address_prefix) + "0.txt"));
		std::string line;
		std::uint64_t address;
		if (std::getline (file, line) && parse_hex (line, address))
		{
			bases.push_back ({ address, executable.string () });
		}
	}
	// Subsequent files hold two lines: the shared library path and its load address
	for (int num = 1;; ++num)
	{
		auto path = base_dir / (std::string (load_address_prefix) + std::to_string (num) + ".txt");
		std::error_code ec;
		if (!std::filesystem::exists (path, ec))
		{
			break;
		}
		std::ifstream file (path);
		std::string library;
		std::string address_line;
		std::getline (file, library);
		std::getline (file, address_line);
		std::uint64_t address;
		if (parse_hex (address_line, address))
		{
			bases.push_back ({ address, library });
		}
	}
	if (bases.empty ())
	{
		return false;
	}
	std::sort (bases.begin (), bases.end (), [] (auto const & a, auto const & b) { return a.address < b.address; });

	// Extract the raw addresses from the (non-symbolicated) stacktrace text
	std::vector<std::uint64_t> addresses;
	{
		std::stringstream ss;
		ss << st;
		std::string line;
		while (std::getline (ss, line))
		{
			std::istringstream iss (line);
			std::string token;
			while (iss >> token)
			{
				std::uint64_t address;
				if (parse_hex (token, address))
				{
					addresses.push_back (address);
					break;
				}
			}
		}
	}
	if (addresses.empty ())
	{
		return false;
	}

	// Capture addr2line output via a temporary file
	auto temp = std::filesystem::temp_directory_path () / ("nano_node_addr2line_" + std::to_string (::getpid ()) + ".txt");
	std::error_code rm_ec;
	std::filesystem::remove (temp, rm_ec);

	int successes = 0;
	auto run_addr2line = [&] (bool relative) {
		for (auto address : addresses)
		{
			for (auto it = bases.rbegin (); it != bases.rend (); ++it)
			{
				if (address > it->address)
				{
					auto target = relative ? address - it->address : address;
					std::stringstream hex;
					hex << std::hex << target;
					auto command = "addr2line -fCi " + hex.str () + " -e " + shell_single_quote (it->library) + " >> " + shell_single_quote (temp.string ()) + " 2>/dev/null";
					if (std::system (command.c_str ()) == 0)
					{
						++successes;
					}
					break;
				}
			}
		}
	};

	run_addr2line (false);
	{
		std::ofstream separator (temp, std::ios::app);
		separator << "\nUsing relative addresses:\n";
	}
	run_addr2line (true);

	if (successes == 0)
	{
		// addr2line is not installed or produced nothing useful
		std::filesystem::remove (temp, rm_ec);
		return false;
	}

	std::ifstream result (temp);
	out << result.rdbuf ();
	result.close ();
	std::filesystem::remove (temp, rm_ec);
	return true;
}
#else
bool symbolicate_binary_dump (std::filesystem::path const &, std::ostream &)
{
	return false;
}
#endif

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

#ifndef _WIN32
namespace
{
struct unwind_capture
{
	static constexpr int max_frames = 256;
	void * frames[max_frames];
	int count = 0;
};

_Unwind_Reason_Code unwind_collect (_Unwind_Context * context, void * arg)
{
	auto * capture = static_cast<unwind_capture *> (arg);
	if (capture->count >= unwind_capture::max_frames)
	{
		return _URC_END_OF_STACK;
	}
	if (auto ip = _Unwind_GetIP (context); ip != 0)
	{
		capture->frames[capture->count++] = reinterpret_cast<void *> (ip);
	}
	return _URC_NO_REASON;
}
}
#endif

namespace
{
void write_stderr (std::string const & text)
{
#ifdef _WIN32
	std::fwrite (text.data (), 1, text.size (), stderr);
	std::fflush (stderr);
#else
	for (std::size_t offset = 0; offset < text.size ();)
	{
		auto written = ::write (STDERR_FILENO, text.data () + offset, text.size () - offset);
		if (written <= 0)
		{
			break;
		}
		offset += static_cast<std::size_t> (written);
	}
#endif
}
}

void nano::dump_crash_stacktrace_readable ()
{
	// Best-effort. Runs after the async-safe binary dump, in the crashing
	// process. Not async-signal-safe, but we are aborting anyway and the binary
	// dump has already succeeded, so a failure here loses nothing.
	std::ostringstream trace;

#ifdef _WIN32
	trace << boost::stacktrace::stacktrace ();
#else
	// boost::stacktrace's libbacktrace backend stops at the signal trampoline,
	// hiding the actual crash site. _Unwind_Backtrace (the unwinder C++
	// exceptions use) steps through the signal frame into the crashing code; we
	// then symbolicate each frame with boost::stacktrace::frame (libbacktrace).
	unwind_capture capture;
	_Unwind_Backtrace (&unwind_collect, &capture);

	int index = 0;
	for (int i = 0; i < capture.count; ++i)
	{
		// Return addresses point just past the call; step back one byte (except
		// the innermost frame) so file/line map to the call site.
		auto raw = reinterpret_cast<std::uintptr_t> (capture.frames[i]);
		boost::stacktrace::frame frame (reinterpret_cast<void *> (i == 0 ? raw : raw - 1));

		auto name = frame.name ();
		// Drop this helper itself so the trace starts at the crash handler/site
		if (index == 0 && name.find ("dump_crash_stacktrace_readable") != std::string::npos)
		{
			continue;
		}

		trace << ' ' << index << "# ";
		if (!name.empty ())
		{
			trace << name;
		}
		else
		{
			trace << "0x" << std::uppercase << std::hex << raw << std::nouppercase << std::dec;
		}

		if (auto source_file = frame.source_file (); !source_file.empty ())
		{
			trace << " at " << source_file << ':' << frame.source_line ();
		}
		else if (Dl_info info; dladdr (capture.frames[i], &info) != 0 && info.dli_fname != nullptr)
		{
			trace << " in " << info.dli_fname;
		}
		trace << '\n';
		++index;
	}
#endif

	auto text = trace.str ();

	// Emit to stderr immediately so the crash is visible in the container log
	// (e.g. `docker logs`) at crash time, not only on the next startup scan.
	write_stderr ("==== Crash stacktrace (caught fatal signal) ====\n" + text + "==== End of crash stacktrace ====\n");

	// Also persist it (plain, no markers) so the next-startup scan can reprint
	// it and so it survives if stderr was not captured.
	std::ofstream ofs (crash_readable_path, std::ios::out | std::ios::trunc);
	if (ofs.is_open ())
	{
		ofs << text;
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
	store_path (crash_directory, directory);
}

char const * nano::crash_stacktrace_directory ()
{
	return crash_directory;
}

std::size_t nano::output_stacktrace_dumps (std::filesystem::path const & data_path, std::ostream & out, bool include_archived, bool archive_after, bool prefer_advanced_decode)
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
		auto emit = [&out] (std::filesystem::path const & path, std::string const & body) {
			out << "==== Crash stacktrace dump: " << path.string () << " ====\n"
				<< body;
			if (body.empty () || body.back () != '\n')
			{
				out << '\n';
			}
			out << "==== End of crash stacktrace dump ====" << std::endl;
		};

		// The crash-time, already-symbolicated readable text
		auto print_readable = [&] () -> bool {
			if (group.readable.empty ())
			{
				return false;
			}
			std::ifstream ifs (group.readable);
			std::stringstream contents;
			contents << ifs.rdbuf ();
			if (contents.str ().empty ())
			{
				return false;
			}
			emit (group.readable, contents.str ());
			return true;
		};

		// Reconstruct from the binary dump: first the advanced addr2line +
		// load-address ceremony, then fall back to raw addresses
		auto print_binary = [&] () -> bool {
			if (group.binary.empty ())
			{
				return false;
			}
			std::ostringstream body;
			bool resolved = symbolicate_binary_dump (group.binary, body);
			if (!resolved)
			{
				std::ifstream ifs (group.binary, std::ios::binary);
				if (ifs.is_open ())
				{
					boost::stacktrace::stacktrace st = boost::stacktrace::stacktrace::from_dump (ifs);
					if (!st.empty ())
					{
						body << st;
						resolved = true;
					}
				}
			}
			if (!resolved || body.str ().empty ())
			{
				return false;
			}
			emit (group.binary, body.str ());
			return true;
		};

		// prefer_advanced_decode flips the order: advanced binary decoding first,
		// readable text only as a fallback when there is no binary dump
		bool emitted = prefer_advanced_decode
		? (print_binary () || print_readable ())
		: (print_readable () || print_binary ());

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
