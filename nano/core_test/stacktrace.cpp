#include <nano/boost/stacktrace.hpp>
#include <nano/lib/files.hpp>
#include <nano/lib/stacktrace.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

// Check that the stacktrace contains the current function name
// This depends on the way testcase names are compiled by gtest
// Current name: "stacktrace_human_readable_Test::TestBody()"
TEST (stacktrace, human_readable)
{
	auto stacktrace = nano::generate_stacktrace ();
	std::cout << stacktrace << std::endl;
	ASSERT_FALSE (stacktrace.empty ());
	ASSERT_TRUE (stacktrace.find ("stacktrace_human_readable_Test") != std::string::npos);
}

// The readable dump is symbolicated in-process and written next to the binary dump
TEST (stacktrace, readable_dump_written_to_path)
{
	auto data_path = nano::unique_path ();
	std::filesystem::create_directories (data_path);
	nano::set_crash_stacktrace_path (data_path);
	nano::dump_crash_stacktrace_readable ();

	auto readable = data_path / "nano_node_backtrace.txt";
	ASSERT_TRUE (std::filesystem::exists (readable));
	std::ifstream ifs (readable);
	std::stringstream ss;
	ss << ifs.rdbuf ();
	auto content = ss.str ();
	ASSERT_FALSE (content.empty ());
	// Captured in this test body, so it must be symbolicated, not raw addresses
	ASSERT_NE (content.find ("readable_dump_written_to_path"), std::string::npos);
}

// A binary-only dump in the data path is found, decoded and printed
TEST (stacktrace, output_dumps_scans_data_path)
{
	auto data_path = nano::unique_path ();
	std::filesystem::create_directories (data_path);
	auto dump_path = data_path / "nano_node_backtrace.dump";
	boost::stacktrace::safe_dump_to (dump_path.string ().c_str ());
	ASSERT_TRUE (std::filesystem::exists (dump_path));

	std::ostringstream out;
	auto printed = nano::output_stacktrace_dumps (data_path, out, /* include_archived */ true, /* archive_after */ false);

	ASSERT_GE (printed, 1);
	ASSERT_NE (out.str ().find (dump_path.string ()), std::string::npos);
	ASSERT_NE (out.str ().find ("Crash stacktrace dump"), std::string::npos);
	// Non-destructive: the dump is still there
	ASSERT_TRUE (std::filesystem::exists (dump_path));
}

// When both halves of a crash exist, the readable one is printed and the pair counts once
TEST (stacktrace, output_dumps_prefers_readable)
{
	auto data_path = nano::unique_path ();
	std::filesystem::create_directories (data_path);
	auto readable_path = data_path / "nano_node_backtrace.txt";
	auto binary_path = data_path / "nano_node_backtrace.dump";

	{
		std::ofstream ofs (readable_path);
		ofs << "MARKER_READABLE_TRACE\n 0# some_symbolicated_frame at file.cpp:42\n";
	}
	boost::stacktrace::safe_dump_to (binary_path.string ().c_str ());

	std::ostringstream out;
	auto printed = nano::output_stacktrace_dumps (data_path, out, /* include_archived */ false, /* archive_after */ true);

	ASSERT_EQ (printed, 1);
	ASSERT_NE (out.str ().find ("MARKER_READABLE_TRACE"), std::string::npos);
	// Binary half must not be decoded/printed when the readable half is present
	ASSERT_EQ (out.str ().find (".dump"), std::string::npos);

	// Both halves archived together so the crash is reported exactly once
	ASSERT_FALSE (std::filesystem::exists (readable_path));
	ASSERT_FALSE (std::filesystem::exists (binary_path));
	bool readable_archived = false;
	bool binary_archived = false;
	for (auto const & entry : std::filesystem::directory_iterator (data_path))
	{
		auto name = entry.path ().filename ().string ();
		if (name.rfind ("nano_node_backtrace.txt.", 0) == 0)
		{
			readable_archived = true;
		}
		if (name.rfind ("nano_node_backtrace.dump.", 0) == 0)
		{
			binary_archived = true;
		}
	}
	ASSERT_TRUE (readable_archived);
	ASSERT_TRUE (binary_archived);
}

// With archive_after, the active dump is renamed aside so it is reported only once
TEST (stacktrace, output_dumps_archives_after)
{
	auto data_path = nano::unique_path ();
	std::filesystem::create_directories (data_path);
	auto dump_path = data_path / "nano_node_backtrace.dump";
	boost::stacktrace::safe_dump_to (dump_path.string ().c_str ());

	std::ostringstream out;
	auto printed = nano::output_stacktrace_dumps (data_path, out, /* include_archived */ false, /* archive_after */ true);
	ASSERT_GE (printed, 1);

	// Active dump has been moved aside
	ASSERT_FALSE (std::filesystem::exists (dump_path));
	bool archived_found = false;
	for (auto const & entry : std::filesystem::directory_iterator (data_path))
	{
		if (entry.path ().filename ().string ().rfind ("nano_node_backtrace.dump.", 0) == 0)
		{
			archived_found = true;
		}
	}
	ASSERT_TRUE (archived_found);

	// A second scan does not re-report the archived dump when include_archived is false
	std::ostringstream out2;
	auto printed2 = nano::output_stacktrace_dumps (data_path, out2, /* include_archived */ false, /* archive_after */ true);
	ASSERT_EQ (printed2, 0);
}
