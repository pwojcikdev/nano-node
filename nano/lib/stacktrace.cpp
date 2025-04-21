#include <nano/lib/files.hpp>
#include <nano/lib/stacktrace.hpp>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/process.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/stacktrace.hpp>

#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{
void nano_abort_signal_handler (int signum)
{
	// remove `signum` from signal handling when under Windows
#ifdef _WIN32
	std::signal (signum, SIG_DFL);
#endif

	// create some debugging log files
	nano::dump_crash_stacktrace ();
	nano::create_load_memory_address_files ();

	// re-raise signal to call the default handler and exit
	raise (signum);
}
}

void nano::install_abort_signal_handler ()
{
	// We catch signal SIGSEGV and SIGABRT * not * via the signal manager because we want these signal handlers
	// to be executed in the stack of the code that caused the signal, so we can dump the stacktrace.
#ifdef _WIN32
	std::signal (SIGSEGV, nano_abort_signal_handler);
	std::signal (SIGABRT, nano_abort_signal_handler);
#else
	struct sigaction sa = {};
	sa.sa_handler = nano_abort_signal_handler;
	sigemptyset (&sa.sa_mask);
	sa.sa_flags = SA_RESETHAND;
	sigaction (SIGSEGV, &sa, NULL);
	sigaction (SIGABRT, &sa, NULL);
#endif
}

void nano::dump_crash_stacktrace ()
{
	boost::stacktrace::safe_dump_to ("nano_node_backtrace.dump");
}

std::string nano::generate_stacktrace ()
{
	auto stacktrace = boost::stacktrace::stacktrace ();
	std::stringstream ss;
	ss << stacktrace;
	return ss.str ();
}

namespace
{
class address_library_pair
{
public:
	uint64_t address;
	std::string library;

	address_library_pair (uint64_t address, std::string library);
	bool operator< (const address_library_pair & other) const;
	bool operator== (const address_library_pair & other) const;
};

class uint64_from_hex // For use with boost::lexical_cast to read hexadecimal strings
{
public:
	uint64_t value;
};
std::istream & operator>> (std::istream & in, uint64_from_hex & out_val);

address_library_pair::address_library_pair (uint64_t address, std::string library) :
	address (address), library (library)
{
}

bool address_library_pair::operator< (const address_library_pair & other) const
{
	return address < other.address;
}

bool address_library_pair::operator== (const address_library_pair & other) const
{
	return address == other.address;
}

std::istream & operator>> (std::istream & in, uint64_from_hex & out_val)
{
	in >> std::hex >> out_val.value;
	return in;
}
}

std::string nano::load_last_crash_stacktrace (std::chrono::seconds max_age)
{
	if (!std::filesystem::exists ("nano_node_backtrace.dump"))
	{
		return {}; // No crash dump file
	}
	if (max_age.count () > 0 && std::filesystem::last_write_time ("nano_node_backtrace.dump") + max_age < std::filesystem::file_time_type::clock::now ())
	{
		return {}; // The crash dump file is too old
	}

	// There is a backtrace, so output the contents
	std::ifstream ifs{ "nano_node_backtrace.dump" };
	boost::stacktrace::stacktrace st = boost::stacktrace::stacktrace::from_dump (ifs);

	std::stringstream ss;

#if defined(_WIN32) // || defined(__APPLE__)
	// Only linux has load addresses, so just write the dump to a readable file.
	// It's the best we can do to keep consistency.
	// TODO: Add support for macOS
	ss << st;
#else
	// Read all the nano node files
	boost::system::error_code err;
	auto running_executable_filepath = boost::dll::program_location (err);
	if (!err)
	{
		auto num = 0;
		auto format = boost::format ("nano_node_crash_load_address_dump_%1%.txt");
		std::vector<address_library_pair> base_addresses;

		// The first one only has the load address
		uint64_from_hex base_address;
		std::string line;
		if (std::filesystem::exists (boost::str (format % num)))
		{
			std::getline (std::ifstream (boost::str (format % num)), line);
			if (boost::conversion::try_lexical_convert (line, base_address))
			{
				base_addresses.emplace_back (base_address.value, running_executable_filepath.string ());
			}
		}
		++num;

		// Now do the rest of the files
		while (std::filesystem::exists (boost::str (format % num)))
		{
			std::ifstream ifs_dump_filename (boost::str (format % num));

			// 2 lines, the path to the dynamic library followed by the load address
			std::string dynamic_lib_path;
			std::getline (ifs_dump_filename, dynamic_lib_path);
			std::getline (ifs_dump_filename, line);

			if (boost::conversion::try_lexical_convert (line, base_address))
			{
				base_addresses.emplace_back (base_address.value, dynamic_lib_path);
			}

			++num;
		}

		std::sort (base_addresses.begin (), base_addresses.end ());

		auto address_column_it = vm.find ("address_column");
		auto column = -1;
		if (address_column_it != vm.end ())
		{
			if (!boost::conversion::try_lexical_convert (address_column_it->second.as<std::string> (), column))
			{
				std::cerr << "Error: Invalid address column\n";
				result = -1;
			}
		}

		// Extract the addresses from the dump file.
		std::stringstream stacktrace_ss;
		stacktrace_ss << st;
		std::vector<uint64_t> backtrace_addresses;
		while (std::getline (stacktrace_ss, line))
		{
			std::istringstream iss (line);
			std::vector<std::string> results (std::istream_iterator<std::string>{ iss }, std::istream_iterator<std::string> ());

			if (column != -1)
			{
				if (column < results.size ())
				{
					uint64_from_hex address_hex;
					if (boost::conversion::try_lexical_convert (results[column], address_hex))
					{
						backtrace_addresses.push_back (address_hex.value);
					}
					else
					{
						std::cerr << "Error: Address column does not point to valid addresses\n";
						result = -1;
					}
				}
				else
				{
					std::cerr << "Error: Address column too high\n";
					result = -1;
				}
			}
			else
			{
				for (auto const & text : results)
				{
					uint64_from_hex address_hex;
					if (boost::conversion::try_lexical_convert (text, address_hex))
					{
						backtrace_addresses.push_back (address_hex.value);
						break;
					}
				}
			}
		}

		std::stringstream crash_report;

		// Hold the results from all addr2line calls, if all fail we can assume that addr2line is not installed,
		// and inform the user that it needs installing
		std::vector<int> system_codes;

		auto run_addr2line = [&backtrace_addresses, &base_addresses, &system_codes, &crash_report] (bool use_relative_addresses) {
			for (auto backtrace_address : backtrace_addresses)
			{
				// Find the closest address to it
				for (auto base_address : boost::adaptors::reverse (base_addresses))
				{
					if (backtrace_address > base_address.address)
					{
						// Addresses need to be in hex for addr2line to work
						auto address = use_relative_addresses ? backtrace_address - base_address.address : backtrace_address;
						std::stringstream ss;
						ss << std::uppercase << std::hex << address;

						// Call addr2line to convert the address into something readable.
						auto res = std::system (boost::str (boost::format ("addr2line -fCi %1% -e %2% >> %3%") % ss.str () % base_address.library % crash_report_filename).c_str ());
						system_codes.push_back (res);
						break;
					}
				}
			}
		};

		// First run addr2line using absolute addresses
		run_addr2line (false);
		{
			std::ofstream ofs (crash_report_filename, std::ios_base::out | std::ios_base::app);
			ofs << std::endl
				<< "Using relative addresses:" << std::endl; // Add an empty line to separate the absolute & relative output
		}

		// Now run using relative addresses. This will give actual results for other dlls, the results from the nano_node executable.
		run_addr2line (true);

		if (std::find (system_codes.begin (), system_codes.end (), 0) == system_codes.end ())
		{
			std::cerr << "Error: Check that addr2line is installed and that nano_node_crash_load_address_dump_*.txt files exist." << std::endl;
			result = -1;
		}
	}
	else
	{
		std::cerr << "Error: Could not determine running executable path" << std::endl;
		result = -1;
	}
#endif
}
