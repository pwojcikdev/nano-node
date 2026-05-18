#include <nano/boost/stacktrace.hpp>
#include <nano/lib/files.hpp>
#include <nano/lib/stacktrace.hpp>

#include <gtest/gtest.h>

#include <filesystem>
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

// A dump written to the data path is found, decoded and printed
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
