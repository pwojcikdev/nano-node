#include <nano/lib/files.hpp>
#include <nano/lib/stats_sinks.hpp>
#include <nano/test_common/scoped_start.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <ostream>

// Test stat counting at both type and detail levels
TEST (stats, counters)
{
	nano::test::system system;
	auto & node = *system.add_node ();

	// Initial state
	ASSERT_EQ (0, node.stats.count (nano::stat::type::ledger, nano::stat::dir::in));
	ASSERT_EQ (0, node.stats.count (nano::stat::type::ledger, nano::stat::detail::test, nano::stat::dir::in));
	ASSERT_EQ (0, node.stats.count (nano::stat::type::ledger, nano::stat::detail::send, nano::stat::dir::out));

	node.stats.add (nano::stat::type::ledger, nano::stat::detail::test, nano::stat::dir::in, 1);
	node.stats.add (nano::stat::type::ledger, nano::stat::detail::test, nano::stat::dir::in, 5);
	node.stats.inc (nano::stat::type::ledger, nano::stat::detail::test, nano::stat::dir::in);
	node.stats.inc (nano::stat::type::ledger, nano::stat::detail::send, nano::stat::dir::in);
	node.stats.inc (nano::stat::type::ledger, nano::stat::detail::send, nano::stat::dir::in);
	node.stats.inc (nano::stat::type::ledger, nano::stat::detail::receive, nano::stat::dir::in);

	ASSERT_EQ (10, node.stats.count (nano::stat::type::ledger, nano::stat::dir::in));
	ASSERT_EQ (2, node.stats.count (nano::stat::type::ledger, nano::stat::detail::send, nano::stat::dir::in));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::ledger, nano::stat::detail::receive, nano::stat::dir::in));

	node.stats.add (nano::stat::type::ledger, nano::stat::detail::test, nano::stat::dir::in, 0);

	ASSERT_EQ (10, node.stats.count (nano::stat::type::ledger, nano::stat::dir::in));
}

TEST (stats, counters_aggregate_all)
{
	nano::test::system system;
	auto & node = *system.add_node ();

	node.stats.add (nano::stat::type::ledger, nano::stat::detail::test, nano::stat::dir::in, 1, true);

	ASSERT_EQ (1, node.stats.count (nano::stat::type::ledger, nano::stat::dir::in));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::ledger, nano::stat::detail::all, nano::stat::dir::in));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::ledger, nano::stat::detail::test, nano::stat::dir::in));

	node.stats.add (nano::stat::type::ledger, nano::stat::detail::activate, nano::stat::dir::in, 5, true);

	ASSERT_EQ (6, node.stats.count (nano::stat::type::ledger, nano::stat::dir::in));
	ASSERT_EQ (6, node.stats.count (nano::stat::type::ledger, nano::stat::detail::all, nano::stat::dir::in));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::ledger, nano::stat::detail::test, nano::stat::dir::in));
}

TEST (stats, samples)
{
	nano::test::system system;
	auto & node = *system.add_node ();

	node.stats.sample (nano::stat::sample::active_election_duration, 5, { 1, 10 });
	node.stats.sample (nano::stat::sample::active_election_duration, 5, { 1, 10 });
	node.stats.sample (nano::stat::sample::active_election_duration, 11, { 1, 10 });
	node.stats.sample (nano::stat::sample::active_election_duration, 37, { 1, 10 });

	node.stats.sample (nano::stat::sample::bootstrap_tag_duration, 2137, { 1, 10 });

	auto samples1 = node.stats.samples (nano::stat::sample::active_election_duration);
	ASSERT_EQ (4, samples1.size ());
	ASSERT_EQ (5, samples1[0]);
	ASSERT_EQ (5, samples1[1]);
	ASSERT_EQ (11, samples1[2]);
	ASSERT_EQ (37, samples1[3]);

	auto samples2 = node.stats.samples (nano::stat::sample::active_election_duration);
	ASSERT_EQ (0, samples2.size ());

	node.stats.sample (nano::stat::sample::active_election_duration, 3, { 1, 10 });

	auto samples3 = node.stats.samples (nano::stat::sample::active_election_duration);
	ASSERT_EQ (1, samples3.size ());
	ASSERT_EQ (3, samples3[0]);

	auto samples4 = node.stats.samples (nano::stat::sample::bootstrap_tag_duration);
	ASSERT_EQ (1, samples4.size ());
	ASSERT_EQ (2137, samples4[0]);
}

TEST (stats, file_logging_respects_configured_streams)
{
	nano::test::system system;
	auto const path = nano::unique_path ();
	auto contents = [] (std::string const & filename) {
		std::ifstream stream{ filename };
		return std::string{ std::istreambuf_iterator<char>{ stream }, {} };
	};

	nano::stats_config counters;
	counters.log_counters_filename = (path / "counters.stat").string ();
	counters.log_samples_filename = (path / "disabled-samples.stat").string ();
	counters.log_counters_interval = 100ms;
	counters.log_headers = false;
	std::ofstream{ counters.log_samples_filename } << "earlier samples";
	{
		nano::stats stats{ system.logger, counters };
		stats.add (nano::stat::type::ledger, nano::stat::detail::test, 7);
		START (stats);
		ASSERT_TIMELY (5s, contents (counters.log_counters_filename).find (",ledger,test,in,7") != std::string::npos);
		nano::stat_json_writer json;
		stats.log_counters (json);
		auto report = json.to_ptree ();
		EXPECT_EQ ("counters", report.get<std::string> ("type", ""));
		EXPECT_FALSE (report.get<std::string> ("created", "").empty ());
	}
	EXPECT_EQ (std::string::npos, contents (counters.log_counters_filename).find ("counters,"));
	EXPECT_EQ ("earlier samples", contents (counters.log_samples_filename));

	nano::stats_config samples;
	samples.log_counters_filename = (path / "disabled-counters.stat").string ();
	samples.log_samples_filename = (path / "samples.stat").string ();
	samples.log_samples_interval = 100ms;
	samples.log_headers = false;
	{
		nano::stats stats{ system.logger, samples };
		stats.sample (nano::stat::sample::active_election_duration, 17);
		START (stats);
		ASSERT_TIMELY (5s, contents (samples.log_samples_filename).find (",active_election_duration,17") != std::string::npos);
		nano::stat_json_writer json;
		stats.log_samples (json);
		auto report = json.to_ptree ();
		EXPECT_EQ ("samples", report.get<std::string> ("type", ""));
		EXPECT_FALSE (report.get<std::string> ("created", "").empty ());
	}
	EXPECT_EQ (std::string::npos, contents (samples.log_samples_filename).find ("samples,"));
	ASSERT_FALSE (std::filesystem::exists (samples.log_counters_filename));
}

TEST (stats, file_logging_subsecond_intervals)
{
	nano::test::system system;
	auto const path = nano::unique_path ();
	nano::stats_config config;
	config.log_counters_filename = (path / "counters.stat").string ();
	config.log_samples_filename = (path / "samples.stat").string ();
	config.log_counters_interval = 100ms;
	config.log_samples_interval = 250ms;

	// With no counters or samples each writeout produces a single header line.
	auto writeouts = [] (std::string const & filename) {
		std::ifstream stream{ filename };
		return std::count (std::istreambuf_iterator<char>{ stream }, {}, '\n');
	};
	nano::stats stats{ system.logger, config };
	START (stats);
	ASSERT_TIMELY (3s, writeouts (config.log_samples_filename) >= 4);
	auto const samples = writeouts (config.log_samples_filename);
	EXPECT_GE (writeouts (config.log_counters_filename), samples + 2);
}

TEST (stats, retain_samples_until_file_write_succeeds)
{
	nano::test::system system;
	nano::stats stats{ system.logger };
	auto const directory = nano::unique_path () / "stats";
	auto const path = directory / "samples.stat";
	nano::stat_file_writer file{ path.string (), false };
	auto const sampler = nano::stat::sample::active_election_duration;
	auto const other = nano::stat::sample::bootstrap_tag_duration;
	stats.sample (sampler, 17);
	stats.sample (other, 9);
	stats.log_samples (file);
	EXPECT_FALSE (file.out ());
	EXPECT_EQ (0, file.entries ());
	EXPECT_EQ ((std::vector<int64_t>{ 9 }), stats.samples (other));

	std::filesystem::create_directory (directory);
	stats.sample (sampler, 23);
	stats.log_samples (file);
	ASSERT_TRUE (file.out ());
	std::ifstream input{ path };
	std::string line;
	ASSERT_TRUE (std::getline (input, line));
	EXPECT_TRUE (line.ends_with (",active_election_duration,17,23"));
	EXPECT_EQ (1, file.entries ());
	EXPECT_TRUE (stats.samples (sampler).empty ());

	// A stream can also fail while it is writing a sample row.
	file.log.close ();
	stats.sample (sampler, 31);
	stats.sample (other, 47);
	stats.log_samples (file);
	EXPECT_FALSE (file.out ());
	EXPECT_EQ (1, file.entries ());
	EXPECT_EQ ((std::vector<int64_t>{ 31 }), stats.samples (sampler));
	EXPECT_EQ ((std::vector<int64_t>{ 47 }), stats.samples (other));
}

TEST (stats, rotate_after_successful_counter_writes)
{
	nano::test::system system;
	nano::stats_config config;
	config.log_rotation_count = 2;
	nano::stats stats{ system.logger, config };
	auto const directory = nano::unique_path () / "stats";
	auto const path = directory / "counters.stat";
	nano::stat_file_writer file{ path.string (), false };
	stats.add (nano::stat::type::ledger, nano::stat::detail::test, 7);
	stats.log_counters (file);
	EXPECT_EQ (0, file.entries ());

	std::filesystem::create_directory (directory);
	stats.log_counters (file);
	stats.log_counters (file);
	std::ifstream input{ path };
	EXPECT_EQ (2, std::count (std::istreambuf_iterator<char>{ input }, {}, '\n'));
	stats.log_counters (file);
	std::ifstream rotated{ path };
	EXPECT_EQ (1, std::count (std::istreambuf_iterator<char>{ rotated }, {}, '\n'));
}
