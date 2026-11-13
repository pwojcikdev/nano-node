#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/files.hpp>
#include <nano/lib/ratios.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/block_rebroadcaster.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/cementing_set.hpp>
#include <nano/node/election_behavior.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/local_block_broadcaster.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/peer_blocklist.hpp>
#include <nano/node/repcrawler.hpp>
#include <nano/node/scheduler/hinted.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/unchecked_map.hpp>
#include <nano/node/vote_generator.hpp>
#include <nano/node/vote_rebroadcaster.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <mach/mach.h>
#include <magic_enum.hpp>
#include <pthread.h>
#include <sys/resource.h>

using namespace std::chrono_literals;

/*
 * Network simulation for tuning vote solicitation, block flooding and election pacing.
 *
 * A network of principal representatives and ordinary nodes runs in one process over loopback TCP. Every parameter is given in live
 * network units and the simulation runs `timescale` times faster: election timing, block rates, durations and link speeds are all
 * scaled by the same factor, so the ratio of traffic to capacity matches the live network while a run takes minutes.
 *
 * Link speeds are per node upload limits in tiers, the topology is a full mesh or capped to a node degree, and the load is a steady
 * stream of sends from many accounts plus competing fork pairs. Disruptions are node restarts during the load and a partition that
 * heals. Health is measured network wide: confirmation latency to the last node, completeness and divergence over time, recovery
 * after the partition, and the traffic and drops it cost.
 *
 * Parameters come from NANO_TEST_SIM_* environment variables, see sim_params::from_env, and each run appends one JSON line to
 * NANO_TEST_SIM_OUT when set. The named tests below run fixed scenarios and assert that the network converges.
 */

namespace
{
using clock_t = std::chrono::steady_clock;

/** Upload limits in live bytes per second, by node class */
struct bandwidth_profile final
{
	std::string name;
	std::size_t rep{ 0 }; // Zero is the node default
	std::deque<std::size_t> nodes; // Ordinary nodes take these tiers round robin
};

bandwidth_profile make_bandwidth_profile (std::string const & name)
{
	constexpr std::size_t kib = 1024;
	constexpr std::size_t mib = 1024 * 1024;
	if (name == "uniform")
	{
		return { name, 0, { 0 } };
	}
	if (name == "mixed")
	{
		return { name, 8 * mib, { 4 * mib, 1 * mib, 256 * kib } };
	}
	if (name == "constrained")
	{
		return { name, 1 * mib, { 512 * kib, 256 * kib, 128 * kib } };
	}
	if (name == "starved")
	{
		return { name, 512 * kib, { 128 * kib, 64 * kib } };
	}
	release_assert (false, "unknown bandwidth profile: " + name);
	return {};
}

struct sim_params final
{
	std::string label{ "sim" };
	uint64_t seed{ 1 };

	// Network
	std::size_t nodes{ 16 };
	std::size_t reps{ 10 }; // The first nodes are principal representatives, one each
	double rep_weight_skew{ 0.8 }; // Rep weights fall off as 1 / rank^skew
	std::string bandwidth{ "uniform" };
	std::size_t degree{ 0 }; // Connections per node, zero for a full mesh
	double timescale{ 10 }; // Simulated time runs this many times faster than live time

	// Load, in live units
	double rate{ 10 }; // Blocks per second published across the network
	double load_seconds{ 300 };
	std::size_t accounts{ 200 }; // Accounts the load is spread over, each publishes a chain of sends
	std::size_t forks{ 0 }; // Competing block pairs published from two nodes at once during the load
	double drain_seconds{ 600 }; // Time after the load for the network to settle before the run counts as failed

	// Disruptions, in live units
	std::size_t restarts{ 0 }; // Ordinary nodes restarted during the load, spread evenly
	double restart_down_seconds{ 30 };
	bool restart_reps{ false }; // Restart representatives instead of ordinary nodes
	double partition_start{ 0 }; // Seconds into the load, zero for no partition
	double partition_seconds{ 60 };
	double partition_minority{ 0.3 }; // Share of rep weight cut off with half of the ordinary nodes

	// Vote solicitation caps, the live network constants by default
	std::size_t solicit_requests{ 50 };
	std::size_t solicit_broadcasts{ 3 };
	std::size_t channel_messages{ 32 }; // Messages per channel and traffic type in one solicitation round
	std::size_t shared_reps{ 0 }; // Extra rep accounts with the lightest weights, hosted on the rep nodes next to their own account

	// Overrides of the build's own live values, durations in live seconds; unset keeps the build's value, set on a build without the constant fails
	std::optional<double> broadcast_delay; // Time from an election's latest flood to its directed broadcast
	std::optional<double> fanout_scale; // Multiplier on the fanout of a rebroadcast flood
	std::optional<double> rebroadcast_cooldown; // Minimum time between two floods of a block by one node
	std::optional<double> flood_interval; // Time between an election's floods of its winner
	std::optional<double> block_interval; // Time between an election's directed broadcasts of its winner
	std::optional<double> passive_duration; // Time an election that started with cached votes only listens before it requests votes
	std::optional<double> request_interval; // Longest time between an election's vote requests

	std::string origin{ "live" }; // How load blocks enter: live as relayed traffic, local as a wallet's own block
	bool scale_bootstrap{ true }; // Bootstrap cooldowns run in live time like everything else, off reproduces the earlier runs

	std::string out;
	std::string backend{ "lmdb" }; // Batch runs on other backends are skipped

	using lookup_t = std::function<std::optional<std::string> (char const * name)>;

	// Parameters from NANO_TEST_SIM_<NAME> environment variables
	static sim_params from_env (sim_params defaults)
	{
		return parse (defaults, [] (char const * name) -> std::optional<std::string> {
			auto const full = std::string{ "NANO_TEST_SIM_" } + name;
			if (auto value = std::getenv (full.c_str ()))
			{
				return std::string{ value };
			}
			return std::nullopt;
		});
	}

	// Parameters from a line of NAME=value words
	static sim_params from_line (sim_params defaults, std::string const & line)
	{
		std::unordered_map<std::string, std::string> values;
		std::istringstream words{ line };
		std::string word;
		while (words >> word)
		{
			auto const eq = word.find ('=');
			release_assert (eq != std::string::npos, "batch words are NAME=value: " + word);
			values[word.substr (0, eq)] = word.substr (eq + 1);
		}
		return parse (defaults, [&values] (char const * name) -> std::optional<std::string> {
			if (auto it = values.find (name); it != values.end ())
			{
				return it->second;
			}
			return std::nullopt;
		});
	}

	static sim_params parse (sim_params defaults, lookup_t const & get)
	{
		auto params = defaults;
		auto get_size = [&] (char const * name, std::size_t & target) {
			if (auto value = get (name))
				target = std::stoull (*value);
		};
		auto get_double = [&] (char const * name, double & target) {
			if (auto value = get (name))
				target = std::stod (*value);
		};
		if (auto value = get ("LABEL"))
			params.label = *value;
		if (auto value = get ("SEED"))
			params.seed = std::stoull (*value);
		get_size ("NODES", params.nodes);
		get_size ("REPS", params.reps);
		get_double ("SKEW", params.rep_weight_skew);
		if (auto value = get ("BANDWIDTH"))
			params.bandwidth = *value;
		get_size ("DEGREE", params.degree);
		get_double ("TIMESCALE", params.timescale);
		get_double ("RATE", params.rate);
		get_double ("LOAD", params.load_seconds);
		get_size ("ACCOUNTS", params.accounts);
		get_size ("FORKS", params.forks);
		get_double ("DRAIN", params.drain_seconds);
		get_size ("RESTARTS", params.restarts);
		get_double ("RESTART_DOWN", params.restart_down_seconds);
		if (auto value = get ("RESTART_REPS"))
			params.restart_reps = *value == "1";
		get_double ("PARTITION", params.partition_start);
		get_double ("PARTITION_LEN", params.partition_seconds);
		get_double ("PARTITION_MINORITY", params.partition_minority);
		get_size ("REQUESTS", params.solicit_requests);
		get_size ("BROADCASTS", params.solicit_broadcasts);
		get_size ("CHANNEL_MESSAGES", params.channel_messages);
		get_size ("SHARED_REPS", params.shared_reps);
		auto get_optional = [&] (char const * name, std::optional<double> & target) {
			if (auto value = get (name))
				target = std::stod (*value);
		};
		get_optional ("BROADCAST_DELAY", params.broadcast_delay);
		get_optional ("FANOUT_SCALE", params.fanout_scale);
		get_optional ("COOLDOWN", params.rebroadcast_cooldown);
		get_optional ("FLOOD_INTERVAL", params.flood_interval);
		get_optional ("BLOCK_INTERVAL", params.block_interval);
		get_optional ("PASSIVE_DURATION", params.passive_duration);
		get_optional ("REQUEST_INTERVAL", params.request_interval);
		if (auto value = get ("ORIGIN"))
			params.origin = *value;
		if (auto value = get ("SCALE_BOOTSTRAP"))
			params.scale_bootstrap = *value == "1";
		if (auto value = get ("OUT"))
			params.out = *value;
		if (auto value = get ("BACKEND"))
			params.backend = *value;
		return params;
	}

	// Live duration in simulated time
	std::chrono::milliseconds sim (double live_seconds) const
	{
		return std::chrono::milliseconds{ static_cast<int64_t> (live_seconds * 1000.0 / timescale) };
	}
};

/** An account publishing a chain of sends, or a pair of competing sends */
struct sim_account final
{
	nano::keypair key;
	std::deque<std::shared_ptr<nano::block>> chain;
	std::shared_ptr<nano::block> fork_a;
	std::shared_ptr<nano::block> fork_b;
};

/** One node of the network with what the simulation tracks about it */
struct sim_node final
{
	std::string name;
	bool rep{ false };
	std::size_t upload{ 0 }; // Live bytes per second, zero for the default
	std::filesystem::path path;
	std::optional<nano::node_config> config; // Kept to recreate the node on restart
	nano::node_flags flags;
	nano::account node_id; // Kept in the data path, so it survives restarts
	nano::uint128_t weight{ 0 }; // Of every rep account the node hosts
	std::shared_ptr<nano::node> node; // Null while restarting

	nano::mutex mutex;
	std::unordered_map<nano::block_hash, clock_t::time_point> cemented; // When each tracked block was cemented here
	// When and from where a block first reached the block processor here, whatever the result, and when it went into the ledger
	struct arrival
	{
		clock_t::time_point time;
		nano::block_source source;
	};
	std::unordered_map<nano::block_hash, arrival> seen;
	std::unordered_map<nano::block_hash, clock_t::time_point> seen_live; // First copy that came as live traffic, a duplicate included
	std::unordered_map<nano::block_hash, arrival> processed;

	// Counters carried over from node instances that were restarted
	std::map<std::tuple<nano::stat::type, nano::stat::detail, nano::stat::dir>, uint64_t> carried;
};

/** Counters summed over every node, read as they are named */
struct stat_key final
{
	char const * name;
	nano::stat::type type;
	nano::stat::detail detail;
	nano::stat::dir dir;
};

std::deque<stat_key> tracked_stats ()
{
	using t = nano::stat::type;
	using d = nano::stat::detail;
	using dir = nano::stat::dir;
	return {
		{ "bytes_out", t::traffic_tcp, d::all, dir::out },
		{ "confirm_req_out", t::message, d::confirm_req, dir::out },
		{ "confirm_ack_out", t::message, d::confirm_ack, dir::out },
		{ "publish_out", t::message, d::publish, dir::out },
		{ "drop_out", t::message_drop, d::all, dir::out },
		{ "drop_confirm_req", t::message_drop, d::confirm_req, dir::out },
		{ "drop_confirm_ack", t::message_drop, d::confirm_ack, dir::out },
		{ "drop_publish", t::message_drop, d::publish, dir::out },
		{ "solicit_rounds", t::vote_solicitor, d::loop, dir::in },
		{ "solicit_requests", t::vote_solicitor, d::request, dir::out },
		{ "solicit_request_hashes", t::vote_solicitor, d::request_hashes, dir::out },
		{ "solicit_request_failed", t::vote_solicitor, d::request_failed, dir::out },
		{ "solicit_broadcasts", t::vote_solicitor, d::broadcast, dir::out },
		{ "solicit_broadcast_failed", t::vote_solicitor, d::broadcast_failed, dir::out },
		{ "solicit_channel_full", t::vote_solicitor, d::channel_full, dir::out },
		{ "solicit_no_targets", t::vote_solicitor, d::no_targets, dir::out },
		{ "solicit_exhausted", t::vote_solicitor, d::exhausted, dir::out },
		{ "publish_in", t::message, d::publish, dir::in },
		{ "confirm_req_in", t::message, d::confirm_req, dir::in },
		{ "elections_started", t::active_elections, d::started, dir::in },
		{ "rebroadcast_floods", t::block_rebroadcaster, d::rebroadcast, dir::in },
		{ "rebroadcast_sent", t::block_rebroadcaster, d::sent, dir::in },
		{ "rebroadcast_cooled", t::block_rebroadcaster, d::already_rebroadcasted, dir::in },
		{ "local_broadcasts", t::local_block_broadcaster, d::broadcast, dir::in },
		{ "local_sent", t::local_block_broadcaster, d::sent, dir::in },
		{ "blocks_live", t::block_processor_source, d::live, dir::in },
		{ "blocks_bootstrap", t::block_processor_source, d::bootstrap, dir::in },
		{ "publish_duplicate", t::filter, d::duplicate_publish_message, dir::in },
		{ "hinted_missing", t::hinting, d::missing_block, dir::in },
		{ "elections_active_at_start", t::active_elections, d::activate_immediately, dir::in },
		{ "topo_fetched", t::bootstrap_topo, d::fetched, dir::in },
	};
}

std::size_t election_outcomes (nano::node & node, nano::stat::type type)
{
	std::size_t result = 0;
	for (auto behavior : magic_enum::enum_values<nano::election_behavior> ())
	{
		result += node.stats.count (type, nano::to_stat_detail (behavior));
	}
	return result;
}

double percentile (std::vector<double> values, double p)
{
	if (values.empty ())
	{
		return -1;
	}
	std::sort (values.begin (), values.end ());
	auto const index = std::min (values.size () - 1, static_cast<std::size_t> (std::ceil (p * values.size ())) - (p > 0 ? 1 : 0));
	return values[index];
}

double cpu_seconds ()
{
	rusage usage{};
	getrusage (RUSAGE_SELF, &usage);
	return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

// CPU seconds used so far by the live threads of this process, by thread name
std::map<std::string, double> thread_cpu ()
{
	std::map<std::string, double> result;
	thread_act_array_t threads;
	mach_msg_type_number_t count = 0;
	if (task_threads (mach_task_self (), &threads, &count) != KERN_SUCCESS)
	{
		return result;
	}
	for (mach_msg_type_number_t i = 0; i < count; ++i)
	{
		thread_basic_info_data_t info{};
		mach_msg_type_number_t size = THREAD_BASIC_INFO_COUNT;
		if (thread_info (threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t> (&info), &size) == KERN_SUCCESS)
		{
			char name[64] = {};
			if (auto handle = pthread_from_mach_thread_np (threads[i]))
			{
				pthread_getname_np (handle, name, sizeof (name));
			}
			result[name[0] ? name : "unnamed"] += info.user_time.seconds + info.user_time.microseconds / 1e6 + info.system_time.seconds + info.system_time.microseconds / 1e6;
		}
		mach_port_deallocate (mach_task_self (), threads[i]);
	}
	vm_deallocate (mach_task_self (), reinterpret_cast<vm_address_t> (threads), count * sizeof (thread_act_t));
	return result;
}

/** A sample of network wide sync taken periodically */
struct sync_sample final
{
	double live_seconds; // Since the load started, in live time
	uint64_t min_cemented;
	uint64_t max_cemented;
	uint64_t max_blocks;
	std::size_t active_elections; // Summed over nodes
	std::size_t nodes_up;
	bool disturbed; // A node was down, partitioned or still catching up
};

class network_sim final
{
public:
	explicit network_sim (sim_params const & params_a) :
		params{ params_a },
		rng{ params_a.seed }
	{
	}

	~network_sim ()
	{
		// Every node stops before the entries its observers write to go away, recreated ones are not known to the system
		for (auto & entry : nodes)
		{
			if (entry->node)
			{
				entry->node->stop ();
			}
		}
	}

	// Runs the scenario and returns the result as a JSON object
	std::string run ()
	{
		build_ledger ();
		build_nodes ();
		connect ();
		wait_for_reps ();
		load ();
		drain ();
		return report ();
	}

	bool converged () const
	{
		return converged_m;
	}

private:
	sim_params const params;
	std::mt19937_64 rng;
	nano::test::system system;
	nano::block_builder builder;

	std::deque<nano::keypair> rep_keys;
	std::deque<nano::uint128_t> rep_weights;
	std::deque<sim_account> accounts; // Load accounts, then fork accounts
	std::deque<std::shared_ptr<nano::block>> seed_blocks;
	std::deque<std::unique_ptr<sim_node>> nodes;

	// Load blocks with the time they went out and whether any node was down or partitioned at that moment
	struct publish_info
	{
		clock_t::time_point time;
		bool disturbed;
		std::size_t origin; // Node the block entered the network at
	};
	std::unordered_map<nano::block_hash, publish_info> published;
	// Competing blocks published from two nodes at once
	struct fork_pair
	{
		std::shared_ptr<nano::block> a;
		std::shared_ptr<nano::block> b;
		clock_t::time_point published;
	};
	std::deque<fork_pair> fork_pairs;
	uint64_t expected_cemented{ 0 };

	clock_t::time_point load_start;
	clock_t::time_point load_end;
	clock_t::time_point drain_end;
	double cpu_start{ 0 };
	double cpu_end{ 0 };
	std::map<std::string, double> threads_start;
	std::map<std::string, double> threads_end;
	std::deque<sync_sample> samples;
	bool converged_m{ false };

	// A node that has to catch up, from a restart or the partition, with the cemented count it has to reach
	struct recovery
	{
		std::deque<std::size_t> members;
		clock_t::time_point since;
		uint64_t target;
		bool partition;
	};
	std::deque<recovery> recovering;
	std::deque<double> restart_recoveries; // Live seconds from a restart until the node held what was cemented when it came back
	std::optional<double> partition_recovery; // Live seconds from the heal until the minority held what was cemented then
	std::size_t partition_unrecovered{ 0 };

	std::unordered_set<std::size_t> publish_excluded; // Nodes that restart during the load never originate blocks

	// Partition state, by node index
	std::deque<bool> minority;
	bool partition_active{ false };

	double live (clock_t::duration duration) const
	{
		return std::chrono::duration<double> (duration).count () * params.timescale;
	}

	/*
	 * Ledger: genesis funds the representatives with skewed weights and the load accounts with a little each
	 */

	void build_ledger ()
	{
		auto const supply = nano::dev::constants.genesis_amount;
		auto const load_count = params.accounts + params.forks;
		nano::uint128_t const account_balance = nano::Knano_ratio;

		// Weights fall off by rank and sum to nearly all of the supply
		double total_share = 0;
		std::deque<double> shares;
		auto const rep_accounts = params.reps + params.shared_reps;
		for (std::size_t i = 0; i < rep_accounts; ++i)
		{
			shares.push_back (1.0 / std::pow (i + 1, params.rep_weight_skew));
			total_share += shares.back ();
		}
		nano::uint128_t const distributable = supply - account_balance * (load_count + 1);
		for (std::size_t i = 0; i < rep_accounts; ++i)
		{
			rep_keys.emplace_back ();
			auto const weight = static_cast<nano::uint128_t> (static_cast<long double> (distributable) * (shares[i] / total_share));
			rep_weights.push_back (weight);
		}

		auto balance = supply;
		auto previous = nano::dev::genesis->hash ();
		auto fund = [&] (nano::keypair const & key, nano::uint128_t amount, nano::account const & representative) {
			balance -= amount;
			auto send = builder.state ()
						.account (nano::dev::genesis_key.pub)
						.previous (previous)
						.representative (nano::dev::genesis_key.pub)
						.balance (balance)
						.link (key.pub)
						.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
						.work (*system.work.generate (previous))
						.build ();
			previous = send->hash ();
			auto open = builder.state ()
						.account (key.pub)
						.previous (0)
						.representative (representative)
						.balance (amount)
						.link (send->hash ())
						.sign (key.prv, key.pub)
						.work (*system.work.generate (key.pub))
						.build ();
			seed_blocks.push_back (send);
			seed_blocks.push_back (open);
			return open;
		};

		for (std::size_t i = 0; i < rep_keys.size (); ++i)
		{
			fund (rep_keys[i], rep_weights[i], rep_keys[i].pub);
		}

		// Chains are built up front so publishing costs nothing during the load
		auto const total_blocks = static_cast<std::size_t> (params.rate * params.load_seconds);
		auto const chain_length = (total_blocks + params.accounts - 1) / std::max<std::size_t> (params.accounts, 1);
		std::size_t spam_blocks = 0;
		for (std::size_t i = 0; i < load_count; ++i)
		{
			sim_account account;
			auto const & representative = rep_keys[i % params.reps].pub;
			auto open = fund (account.key, account_balance, representative);
			auto head = open->hash ();
			nano::uint128_t account_left = account_balance;
			if (i < params.accounts)
			{
				for (std::size_t j = 0; j < chain_length && spam_blocks < total_blocks; ++j, ++spam_blocks)
				{
					account_left -= 1;
					auto send = builder.state ()
								.account (account.key.pub)
								.previous (head)
								.representative (representative)
								.balance (account_left)
								.link (nano::keypair ().pub)
								.sign (account.key.prv, account.key.pub)
								.work (*system.work.generate (head))
								.build ();
					head = send->hash ();
					account.chain.push_back (send);
				}
			}
			else
			{
				auto make_fork = [&] () {
					return builder.state ()
					.account (account.key.pub)
					.previous (head)
					.representative (representative)
					.balance (account_left - 1)
					.link (nano::keypair ().pub)
					.sign (account.key.prv, account.key.pub)
					.work (*system.work.generate (head))
					.build ();
				};
				account.fork_a = make_fork ();
				account.fork_b = make_fork ();
			}
			accounts.push_back (std::move (account));
		}

		expected_cemented = 1 + seed_blocks.size () + spam_blocks + params.forks;
		system.set_cemented_initialization_blocks (seed_blocks);
	}

	/*
	 * Nodes: the first ones vote, link speeds by profile, timing scaled
	 */

	nano::node_config make_config (sim_node const & entry, nano::uint128_t total_weight)
	{
		auto config = system.default_config ();
		auto & network = config.network_params.network;
		auto const scaled = [this] (std::chrono::milliseconds live_value) {
			return std::chrono::milliseconds{ std::max<int64_t> (1, static_cast<int64_t> (live_value.count () / params.timescale)) };
		};
		network.aec_loop_interval = scaled (300ms);
		// The build's own live election and flooding timings, scaled, so that a comparison of builds measures the builds
		nano::network_constants const live{ nano::work_thresholds::publish_full, nano::network_type::nano_live_network };
		network.election_base_latency = scaled (live.election_base_latency);
		network.vote_broadcast_interval = scaled (live.vote_broadcast_interval);
		network.block_broadcast_interval = scaled (live.block_broadcast_interval);
		network.block_broadcast_delay = scaled (live.block_broadcast_delay);
		auto const scale_optional = [&scaled] (auto & target, auto const & source) {
			if constexpr (requires { target.block_flood_interval; })
			{
				target.block_flood_interval = scaled (source.block_flood_interval);
			}
			if constexpr (requires { target.election_passive_duration; })
			{
				target.election_passive_duration = scaled (source.election_passive_duration);
			}
			if constexpr (requires { target.election_request_interval; })
			{
				target.election_request_interval = scaled (source.election_request_interval);
			}
		};
		scale_optional (network, live);
		config.block_rebroadcaster->rebroadcast_cooldown = scaled (config.block_rebroadcaster->rebroadcast_cooldown);

		// Overrides given in live seconds
		auto const override_live = [&scaled] (std::optional<double> const & live_seconds, std::chrono::milliseconds & target) {
			if (live_seconds)
			{
				target = scaled (std::chrono::milliseconds{ static_cast<int64_t> (*live_seconds * 1000) });
			}
		};
		override_live (params.broadcast_delay, network.block_broadcast_delay);
		override_live (params.block_interval, network.block_broadcast_interval);
		override_live (params.rebroadcast_cooldown, config.block_rebroadcaster->rebroadcast_cooldown);
		auto const override_optional = [&] (auto & target, auto & rebroadcaster) {
			if constexpr (requires { target.block_flood_interval; })
			{
				override_live (params.flood_interval, target.block_flood_interval);
			}
			else
			{
				release_assert (!params.flood_interval, "this build has no block_flood_interval");
			}
			if constexpr (requires { target.election_passive_duration; })
			{
				override_live (params.passive_duration, target.election_passive_duration);
			}
			else
			{
				release_assert (!params.passive_duration, "this build has no election_passive_duration");
			}
			if constexpr (requires { target.election_request_interval; })
			{
				override_live (params.request_interval, target.election_request_interval);
			}
			else
			{
				release_assert (!params.request_interval, "this build has no election_request_interval");
			}
			if constexpr (requires { rebroadcaster.fanout_scale; })
			{
				if (params.fanout_scale)
				{
					rebroadcaster.fanout_scale = *params.fanout_scale;
				}
			}
			else
			{
				release_assert (!params.fanout_scale, "this build has no block_rebroadcaster fanout_scale");
			}
		};
		override_optional (network, *config.block_rebroadcaster);
		// Background chatter at live rates, the dev network runs it far more often
		auto const scaled_seconds = [&scaled] (std::chrono::seconds live_value) {
			return std::max<std::chrono::seconds> (1s, std::chrono::duration_cast<std::chrono::seconds> (scaled (live_value)));
		};
		network.keepalive_period = scaled_seconds (15s);
		network.cleanup_period = scaled_seconds (60s);
		network.idle_timeout = scaled_seconds (120s);
		network.telemetry_request_cooldown = scaled (15s);
		network.telemetry_request_interval = scaled (60s);
		network.telemetry_broadcast_interval = scaled (60s);
		network.telemetry_cache_cutoff = scaled (130s);
		network.rep_crawler_normal_interval = scaled (7s);
		network.rep_crawler_warmup_interval = scaled (3s);
		network.rep_crawler_query_interval = scaled (60s);
		config.network_params.solicitation.election_vote_requests = params.solicit_requests;
		config.network_params.solicitation.election_block_broadcasts = params.solicit_broadcasts;
		config.network_params.solicitation.channel_messages = params.channel_messages;
		config.vote_generator->delay = scaled (100ms);
		config.vote_rebroadcaster->rebroadcast_threshold = scaled (90s);
		config.active_elections->stale_threshold = std::max<std::chrono::seconds> (1s, std::chrono::duration_cast<std::chrono::seconds> (scaled (60s)));
		config.hinted_scheduler->check_interval = scaled (1000ms);
		config.hinted_scheduler->block_cooldown = scaled (10s);
		if (params.scale_bootstrap)
		{
			// How long a node takes to recover a block the flood never brought
			config.bootstrap->account_sets.cooldown = scaled (3s);
			config.bootstrap->frontier_scan.cooldown = scaled (5s);
			config.bootstrap->topo_scan.cooldown = scaled (5s);
			config.bootstrap->topo_scan.fetch_cooldown = scaled (2s);
			config.bootstrap->request_timeout = scaled (15s);
		}
		config.local_block_broadcaster->rebroadcast_interval = scaled_seconds (3s);
		config.local_block_broadcaster->max_rebroadcast_interval = scaled_seconds (60s);
		config.local_block_broadcaster->cleanup_interval = scaled_seconds (60s);

		// Every node of the network shares this machine, thread pools sized for a machine of its own only contend
		config.io_threads = 2;
		config.network_threads = 2;
		config.background_threads = 2;
		config.work_threads = 1;
		config.signature_checker_threads = 1;
		config.wallet_threads = 1;

		config.enable_voting = entry.rep;
		config.online_weight_minimum = total_weight; // Quorum is two thirds of all rep weight, whoever is online

		// Upload limits and their burst scale with time, zero is unlimited
		config.bandwidth_limit = static_cast<std::size_t> (entry.upload * params.timescale);
		config.bandwidth_limit_burst_ratio = std::max (1.0, 3.0 / params.timescale);
		config.bootstrap_bandwidth_limit = static_cast<std::size_t> (5 * 1024 * 1024 * params.timescale);
		if (params.degree > 0)
		{
			config.network->max_peers_per_ip = params.degree; // Every peer is on loopback, so this caps the node's connections
		}
		return config;
	}

	void build_nodes ()
	{
		auto const profile = make_bandwidth_profile (params.bandwidth);
		nano::uint128_t total_weight = 0;
		for (auto const & weight : rep_weights)
		{
			total_weight += weight;
		}

		for (std::size_t i = 0; i < params.nodes; ++i)
		{
			auto entry = std::make_unique<sim_node> ();
			entry->rep = i < params.reps;
			entry->name = entry->rep ? "rep_" + std::to_string (i) : "node_" + std::to_string (i - params.reps);
			entry->upload = entry->rep ? profile.rep : profile.nodes[(i - params.reps) % profile.nodes.size ()];
			entry->config.emplace (make_config (*entry, total_weight));
			entry->flags.disable_max_peers_per_subnetwork = true;
			entry->flags.disable_max_peers_per_ip = params.degree == 0;
			entry->flags.all_reps_principal = false; // Principal by weight as on the live network

			entry->node = system.make_disconnected_node (*entry->config, entry->flags);
			entry->path = entry->node->application_path;
			entry->node_id = entry->node->get_node_id ();
			if (entry->rep)
			{
				auto wallet = entry->node->wallets.create (nano::random_wallet_id ());
				wallet->insert_adhoc (rep_keys[i].prv);
				entry->weight = rep_weights[i];
				// Shared accounts go round robin over the rep nodes, the heaviest node hosts the heaviest extra
				for (std::size_t k = params.reps + i; k < rep_keys.size (); k += params.reps)
				{
					wallet->insert_adhoc (rep_keys[k].prv);
					entry->weight += rep_weights[k];
				}
			}
			observe (*entry);
			nodes.push_back (std::move (entry));
		}
	}

	void observe (sim_node & entry)
	{
		auto * entry_l = &entry;
		entry.node->cementing_set.batch_cemented.add ([entry_l] (auto const & batch) {
			auto const now = clock_t::now ();
			nano::lock_guard<nano::mutex> guard{ entry_l->mutex };
			for (auto const & context : batch)
			{
				entry_l->cemented.emplace (context.block->hash (), now);
			}
		});
		entry.node->ledger_notifications.blocks_processed.add ([entry_l] (auto const & batch) {
			auto const now = clock_t::now ();
			nano::lock_guard<nano::mutex> guard{ entry_l->mutex };
			for (auto const & [result, context] : batch)
			{
				auto const hash = context.block->hash ();
				entry_l->seen.emplace (hash, sim_node::arrival{ now, context.source });
				if (context.source == nano::block_source::live || context.source == nano::block_source::live_originator)
				{
					entry_l->seen_live.emplace (hash, now);
				}
				if (result == nano::block_status::progress)
				{
					entry_l->processed.emplace (hash, sim_node::arrival{ now, context.source });
				}
			}
		});
	}

	void connect ()
	{
		if (params.degree == 0)
		{
			for (std::size_t i = 0; i < nodes.size (); ++i)
			{
				for (std::size_t j = i + 1; j < nodes.size (); ++j)
				{
					nodes[i]->node->network.merge_peer (nodes[j]->node->network.endpoint ());
				}
			}
			return;
		}
		// A ring keeps the graph connected, random chords and reachout fill the remaining degree
		for (std::size_t i = 0; i < nodes.size (); ++i)
		{
			nodes[i]->node->network.merge_peer (nodes[(i + 1) % nodes.size ()]->node->network.endpoint ());
		}
		std::uniform_int_distribution<std::size_t> pick (0, nodes.size () - 1);
		for (std::size_t i = 0; i < nodes.size (); ++i)
		{
			for (std::size_t k = 0; k + 2 < params.degree; k += 2)
			{
				auto const j = pick (rng);
				if (j != i)
				{
					nodes[i]->node->network.merge_peer (nodes[j]->node->network.endpoint ());
				}
			}
		}
	}

	// Every node knows every representative before the load starts, in a capped mesh at least enough weight for quorum
	void wait_for_reps ()
	{
		auto const deadline = clock_t::now () + 120s;
		nano::uint128_t total = 0;
		for (auto const & weight : rep_weights)
		{
			total += weight;
		}
		for (auto & entry : nodes)
		{
			while (true)
			{
				auto const reps = entry->node->rep_crawler.principal_representatives ();
				if (params.degree == 0 ? reps.size () >= rep_keys.size () : entry->node->rep_crawler.total_weight () > total / 3 * 2)
				{
					break;
				}
				release_assert (clock_t::now () < deadline, "representatives were not discovered");
				std::this_thread::sleep_for (50ms);
			}
		}
		std::this_thread::sleep_for (params.sim (5));
	}

	/*
	 * Load: blocks go out at the target rate from random nodes, disruptions happen on schedule
	 */

	struct restart_event
	{
		clock_t::duration down_at;
		clock_t::duration up_at;
		std::size_t node;
		bool down{ false };
		bool up{ false };
	};

	void load ()
	{
		// Publish order: one block from every account in turn
		std::deque<std::shared_ptr<nano::block>> order;
		std::size_t longest = 0;
		for (std::size_t i = 0; i < params.accounts; ++i)
		{
			longest = std::max (longest, accounts[i].chain.size ());
		}
		for (std::size_t j = 0; j < longest; ++j)
		{
			for (std::size_t i = 0; i < params.accounts; ++i)
			{
				if (j < accounts[i].chain.size ())
				{
					order.push_back (accounts[i].chain[j]);
				}
			}
		}

		std::deque<std::pair<clock_t::duration, std::size_t>> fork_times; // Moment and fork account index
		for (std::size_t f = 0; f < params.forks; ++f)
		{
			fork_times.emplace_back (params.sim (params.load_seconds * (f + 0.5) / params.forks), params.accounts + f);
		}

		// Each candidate restarts at most once, so down windows never overlap on a node
		std::deque<std::size_t> candidates;
		for (std::size_t i = 0; i < nodes.size (); ++i)
		{
			if (nodes[i]->rep == params.restart_reps)
			{
				candidates.push_back (i);
			}
		}
		std::shuffle (candidates.begin (), candidates.end (), rng);
		std::deque<restart_event> restarts;
		auto const restart_count = std::min (params.restarts, candidates.size ());
		for (std::size_t r = 0; r < restart_count; ++r)
		{
			auto const at = params.load_seconds * (r + 1) / (restart_count + 1);
			restarts.push_back ({ params.sim (at), params.sim (at + params.restart_down_seconds), candidates[r] });
			// A block handed to a node about to stop would be lost with it, wallets would publish it again, the simulation does not
			publish_excluded.insert (candidates[r]);
		}

		bool partition_done = params.partition_start <= 0;
		std::uniform_int_distribution<std::size_t> pick_node (0, nodes.size () - 1);

		cpu_start = cpu_seconds ();
		threads_start = thread_cpu ();
		load_start = clock_t::now ();
		auto const interval = std::chrono::duration<double> (1.0 / (params.rate * params.timescale));
		// After a slow step the backlog is published over time instead of in one burst
		auto const max_per_tick = std::max<std::size_t> (1, static_cast<std::size_t> (std::ceil (0.05 / interval.count ())));
		std::size_t next = 0;
		auto next_sample = load_start;
		auto const load_length = params.sim (params.load_seconds);
		while (true)
		{
			auto const now = clock_t::now ();
			auto const elapsed = now - load_start;
			bool const restarts_done = std::all_of (restarts.begin (), restarts.end (), [] (auto const & event) { return event.up; });
			if (elapsed >= load_length && next >= order.size () && fork_times.empty () && restarts_done && partition_done)
			{
				break;
			}

			auto const due = std::min<std::size_t> (order.size (), static_cast<std::size_t> (std::chrono::duration<double> (elapsed) / interval) + 1);
			for (std::size_t sent = 0; next < due && sent < max_per_tick; ++next, ++sent)
			{
				publish (order[next], pick_up_node (pick_node), true);
			}

			while (!fork_times.empty () && elapsed >= fork_times.front ().first)
			{
				auto const & account = accounts[fork_times.front ().second];
				auto const a = pick_up_node (pick_node);
				auto b = pick_up_node (pick_node);
				for (int attempt = 0; b == a && attempt < 10; ++attempt)
				{
					b = pick_up_node (pick_node);
				}
				publish (account.fork_a, a, false);
				publish (account.fork_b, b, false);
				fork_pairs.push_back ({ account.fork_a, account.fork_b, clock_t::now () });
				fork_times.pop_front ();
			}

			for (auto & event : restarts)
			{
				if (!event.down && elapsed >= event.down_at)
				{
					take_down (event.node);
					event.down = true;
				}
				if (event.down && !event.up && elapsed >= event.up_at)
				{
					bring_up (event.node);
					event.up = true;
				}
			}

			if (!partition_done && !partition_active && elapsed >= params.sim (params.partition_start))
			{
				set_partition (true);
			}
			if (partition_active && elapsed >= params.sim (params.partition_start + params.partition_seconds))
			{
				set_partition (false);
				partition_done = true;
			}

			track_recoveries ();
			if (now >= next_sample)
			{
				sample ();
				next_sample = now + params.sim (1);
			}
			std::this_thread::sleep_for (5ms);
		}
		load_end = clock_t::now ();
	}

	bool disturbed () const
	{
		return partition_active || std::any_of (nodes.begin (), nodes.end (), [] (auto const & entry) { return !entry->node; }) || !recovering.empty ();
	}

	std::size_t pick_up_node (std::uniform_int_distribution<std::size_t> & pick)
	{
		while (true)
		{
			auto const index = pick (rng);
			if (nodes[index]->node && !publish_excluded.contains (index))
			{
				return index;
			}
		}
	}

	// Load blocks are tracked for latency, fork blocks only for which side wins
	void publish (std::shared_ptr<nano::block> const & block, std::size_t index, bool track)
	{
		if (track)
		{
			published.emplace (block->hash (), publish_info{ clock_t::now (), disturbed (), index });
		}
		if (params.origin == "local")
		{
			// As a wallet or the process RPC hands it over: the local broadcaster sends it to every principal representative and a fanout of other peers
			nodes[index]->node->block_processor.add (block->clone (), nano::block_source::local);
		}
		else
		{
			nodes[index]->node->process_active (block->clone ());
		}
	}

	uint64_t max_cemented_now () const
	{
		uint64_t result = 0;
		for (auto const & entry : nodes)
		{
			if (entry->node)
			{
				result = std::max (result, entry->node->ledger.cemented_count ());
			}
		}
		return result;
	}

	/*
	 * Restarts: the node stops, its store closes, and a new instance opens the same data path later
	 */

	void take_down (std::size_t index)
	{
		auto & entry = *nodes[index];
		carry_stats (entry);
		entry.node->stop ();
		// The system keeps no reference, so the store closes before the data path is opened again
		std::erase (system.disconnected_nodes, entry.node);
		entry.node.reset ();
	}

	void bring_up (std::size_t index)
	{
		auto & entry = *nodes[index];
		entry.node = std::make_shared<nano::node> (entry.path, *entry.config, system.work, entry.flags, 1000 + index);
		observe (entry);
		if (partition_active)
		{
			apply_partition (index, true);
		}
		entry.node->start ();
		// It reconnects through a few reachable peers and learns the rest from them
		std::deque<std::size_t> peers;
		for (std::size_t i = 0; i < nodes.size (); ++i)
		{
			if (i != index && nodes[i]->node && (!partition_active || minority[i] == minority[index]))
			{
				peers.push_back (i);
			}
		}
		std::shuffle (peers.begin (), peers.end (), rng);
		for (std::size_t k = 0; k < peers.size () && k < 3; ++k)
		{
			entry.node->network.merge_peer (nodes[peers[k]]->node->network.endpoint ());
		}
		recovering.push_back ({ { index }, clock_t::now (), max_cemented_now (), false });
	}

	void carry_stats (sim_node & entry)
	{
		for (auto const & key : tracked_stats ())
		{
			entry.carried[{ key.type, key.detail, key.dir }] += entry.node->stats.count (key.type, key.detail, key.dir);
		}
		for (auto type : { nano::stat::type::active_elections_confirmed, nano::stat::type::active_elections_timeout, nano::stat::type::active_elections_dropped })
		{
			entry.carried[{ type, nano::stat::detail::all, nano::stat::dir::in }] += election_outcomes (*entry.node, type);
		}
	}

	/*
	 * Partition: the minority holds a share of the rep weight and half of the ordinary nodes, cut off by node id in both directions
	 */

	void choose_minority ()
	{
		nano::uint128_t total = 0;
		for (auto const & weight : rep_weights)
		{
			total += weight;
		}
		minority.assign (nodes.size (), false);
		// Smallest reps first until the minority holds its share of the weight
		nano::uint128_t held = 0;
		for (std::size_t i = params.reps; i-- > 0;)
		{
			if (static_cast<double> (held + nodes[i]->weight) > params.partition_minority * static_cast<double> (total))
			{
				break;
			}
			held += nodes[i]->weight;
			minority[i] = true;
		}
		for (std::size_t i = params.reps; i < nodes.size (); i += 2)
		{
			minority[i] = true;
		}
	}

	// Blocks or unblocks every node on the other side for the node at index, by the ids saved at build time
	void apply_partition (std::size_t index, bool active)
	{
		auto & node = *nodes[index]->node;
		for (std::size_t j = 0; j < nodes.size (); ++j)
		{
			if (minority[index] == minority[j])
			{
				continue;
			}
			if (active)
			{
				node.peer_blocklist.add (nodes[j]->node_id);
				if (auto channel = node.network.find_node_id (nodes[j]->node_id))
				{
					channel->close ();
				}
			}
			else
			{
				node.peer_blocklist.remove (nodes[j]->node_id);
			}
		}
	}

	void set_partition (bool active)
	{
		if (minority.empty ())
		{
			choose_minority ();
		}
		partition_active = active;
		for (std::size_t i = 0; i < nodes.size (); ++i)
		{
			if (nodes[i]->node)
			{
				apply_partition (i, active);
			}
		}
		if (!active)
		{
			// Healing peers find each other again through direct contacts
			std::deque<std::size_t> members;
			for (std::size_t i = 0; i < nodes.size (); ++i)
			{
				for (std::size_t j = i + 1; j < nodes.size (); ++j)
				{
					if (minority[i] != minority[j] && nodes[i]->node && nodes[j]->node)
					{
						// Started directly, reachout remembers the attempts refused during the partition for several cleanup periods
						nodes[i]->node->network.tcp_channels.start_tcp (nodes[j]->node->network.endpoint ());
					}
				}
				if (minority[i])
				{
					members.push_back (i);
				}
			}
			recovering.push_back ({ members, clock_t::now (), max_cemented_now (), true });
		}
	}

	/*
	 * Drain and measurement
	 */

	// A node or the minority has recovered once it holds everything that was cemented anywhere when it came back
	void track_recoveries ()
	{
		auto const now = clock_t::now ();
		for (auto it = recovering.begin (); it != recovering.end ();)
		{
			bool const caught_up = std::all_of (it->members.begin (), it->members.end (), [this, &it] (auto index) {
				return nodes[index]->node && nodes[index]->node->ledger.cemented_count () >= it->target;
			});
			if (caught_up)
			{
				if (it->partition)
				{
					partition_recovery = live (now - it->since);
				}
				else
				{
					restart_recoveries.push_back (live (now - it->since));
				}
				it = recovering.erase (it);
			}
			else
			{
				++it;
			}
		}
	}

	void sample ()
	{
		sync_sample s{};
		s.live_seconds = live (clock_t::now () - load_start);
		s.min_cemented = std::numeric_limits<uint64_t>::max ();
		for (auto const & entry : nodes)
		{
			if (!entry->node)
			{
				continue;
			}
			auto const cemented = entry->node->ledger.cemented_count ();
			s.min_cemented = std::min (s.min_cemented, cemented);
			s.max_cemented = std::max (s.max_cemented, cemented);
			s.max_blocks = std::max (s.max_blocks, entry->node->ledger.block_count ());
			s.active_elections += entry->node->active.size ();
			++s.nodes_up;
		}
		s.disturbed = disturbed ();
		samples.push_back (s);
	}

	// Number of fork pairs resolved the same way on every node, and of pairs where nodes cemented different sides
	std::pair<std::size_t, std::size_t> fork_state () const
	{
		std::size_t resolved = 0;
		std::size_t split = 0;
		for (auto const & pair : fork_pairs)
		{
			auto const & a = pair.a;
			auto const & b = pair.b;
			std::size_t on_a = 0;
			std::size_t on_b = 0;
			for (auto const & entry : nodes)
			{
				if (!entry->node)
				{
					continue;
				}
				auto tx = entry->node->ledger.tx_begin_read ();
				on_a += entry->node->ledger.cemented.block_exists_or_pruned (tx, a->hash ()) ? 1 : 0;
				on_b += entry->node->ledger.cemented.block_exists_or_pruned (tx, b->hash ()) ? 1 : 0;
			}
			if ((on_a == nodes.size () && on_b == 0) || (on_b == nodes.size () && on_a == 0))
			{
				++resolved;
			}
			if (on_a > 0 && on_b > 0)
			{
				++split;
			}
		}
		return { resolved, split };
	}

	// Every node cemented every block and the same side of every fork
	bool all_synced () const
	{
		bool const counts = std::all_of (nodes.begin (), nodes.end (), [this] (auto const & entry) {
			return entry->node && entry->node->ledger.cemented_count () == expected_cemented;
		});
		return counts && fork_state ().first == fork_pairs.size ();
	}

	void drain ()
	{
		auto const deadline = load_end + params.sim (params.drain_seconds);
		auto next_sample = clock_t::now ();
		while (clock_t::now () < deadline)
		{
			track_recoveries ();
			if (all_synced ())
			{
				converged_m = true;
				break;
			}
			if (clock_t::now () >= next_sample)
			{
				sample ();
				next_sample = clock_t::now () + params.sim (1);
			}
			std::this_thread::sleep_for (10ms);
		}
		track_recoveries ();
		sample ();
		drain_end = clock_t::now ();
		cpu_end = cpu_seconds ();
		threads_end = thread_cpu ();
	}

	/*
	 * Propagation: how a block published while the network was whole reached every node other than the one it entered at.
	 * The flood missed a pair of node and block when no copy came as live traffic within a second of live time. The node itself
	 * was late when no copy from any source came within that second, bootstrap can bring a block before the flood does.
	 * The time from a block entering a node's ledger until it is cemented there tells propagation apart from confirmation.
	 */
	void report_propagation (std::function<std::ostringstream &(std::string const &)> const & field)
	{
		constexpr double window = 1.0; // Live seconds
		constexpr double slow_confirmation = 2.0;

		std::size_t pairs = 0;
		std::size_t flood_missed = 0; // No live copy within the window
		std::size_t flood_never = 0; // No live copy at all
		std::size_t flood_missed_reps = 0;
		std::size_t late = 0; // No copy from any source within the window
		std::size_t late_reps = 0;
		std::size_t late_by_live = 0; // A late first copy that was live traffic
		std::size_t late_by_bootstrap = 0;
		std::size_t late_by_other = 0;
		std::size_t late_5s = 0;
		std::size_t never = 0;
		std::size_t bootstrap_first = 0; // First copy came from bootstrap, in time or not
		std::size_t stalled = 0; // In time, but in the ledger more than the window later: behind a missing predecessor
		std::size_t blocks = 0;
		std::size_t blocks_flood_missed = 0; // Blocks the flood did not bring to at least one node
		std::size_t blocks_late = 0; // Blocks at least one node had late
		std::size_t confirmation_slow = 0; // Pairs cemented more than slow_confirmation after the block was in the ledger
		std::size_t confirmation_slow_reps = 0;
		std::vector<double> arrival; // Per pair, publish to first copy
		std::vector<double> arrival_all; // Per block, publish until the last node had a copy
		std::vector<double> recovery; // Per late pair, publish to first copy
		std::vector<double> confirmation; // Per pair, in the ledger to cemented
		std::vector<double> cemented_clean; // Per block, publish to cemented everywhere, blocks every node had in time
		std::vector<double> cemented_late; // The same for blocks at least one node had late
		std::vector<std::pair<double, bool>> cemented_tail; // Publish to cemented everywhere with whether a node had the block late

		for (auto const & [hash, info] : published)
		{
			if (info.disturbed)
			{
				continue;
			}
			++blocks;
			bool block_flood_missed = false;
			bool block_late = false;
			bool everywhere = true;
			double last_arrival = 0;
			double last_cemented = 0;
			for (std::size_t i = 0; i < nodes.size (); ++i)
			{
				auto & entry = *nodes[i];
				nano::lock_guard<nano::mutex> guard{ entry.mutex };
				auto const cemented = entry.cemented.find (hash);
				if (cemented != entry.cemented.end ())
				{
					last_cemented = std::max (last_cemented, live (cemented->second - info.time));
				}
				else
				{
					everywhere = false;
				}
				if (auto const done = entry.processed.find (hash); done != entry.processed.end () && cemented != entry.cemented.end ())
				{
					auto const delay = live (cemented->second - done->second.time);
					confirmation.push_back (delay);
					confirmation_slow += delay > slow_confirmation ? 1 : 0;
					confirmation_slow_reps += delay > slow_confirmation && entry.rep ? 1 : 0;
				}
				if (i == info.origin)
				{
					continue;
				}
				++pairs;

				auto const live_copy = entry.seen_live.find (hash);
				bool const flooded = live_copy != entry.seen_live.end () && live (live_copy->second - info.time) <= window;
				if (!flooded)
				{
					++flood_missed;
					flood_never += live_copy == entry.seen_live.end () ? 1 : 0;
					flood_missed_reps += entry.rep ? 1 : 0;
					block_flood_missed = true;
				}

				auto const seen = entry.seen.find (hash);
				if (seen == entry.seen.end ())
				{
					++never;
					++late;
					block_late = true;
					continue;
				}
				auto const latency = live (seen->second.time - info.time);
				arrival.push_back (latency);
				last_arrival = std::max (last_arrival, latency);
				bootstrap_first += seen->second.source == nano::block_source::bootstrap ? 1 : 0;
				if (latency > window)
				{
					++late;
					late_reps += entry.rep ? 1 : 0;
					late_5s += latency > 5.0 ? 1 : 0;
					block_late = true;
					recovery.push_back (latency);
					if (seen->second.source == nano::block_source::live || seen->second.source == nano::block_source::live_originator)
					{
						++late_by_live;
					}
					else if (seen->second.source == nano::block_source::bootstrap)
					{
						++late_by_bootstrap;
					}
					else
					{
						++late_by_other;
					}
				}
				else if (auto const done = entry.processed.find (hash); done != entry.processed.end ())
				{
					stalled += live (done->second.time - info.time) > latency + window ? 1 : 0;
				}
			}
			arrival_all.push_back (last_arrival);
			blocks_flood_missed += block_flood_missed ? 1 : 0;
			blocks_late += block_late ? 1 : 0;
			if (everywhere)
			{
				(block_late ? cemented_late : cemented_clean).push_back (last_cemented);
				cemented_tail.emplace_back (last_cemented, block_late);
			}
		}

		auto const share = [] (std::size_t part, std::size_t whole) {
			return whole > 0 ? static_cast<double> (part) / static_cast<double> (whole) : 0.0;
		};
		auto const count_over = [] (std::vector<double> const & values, double limit) {
			return static_cast<std::size_t> (std::count_if (values.begin (), values.end (), [limit] (double value) { return value > limit; }));
		};
		auto const largest = [] (std::vector<double> const & values) {
			return values.empty () ? -1.0 : *std::max_element (values.begin (), values.end ());
		};
		field ("prop_pairs") << pairs;
		field ("prop_blocks") << blocks;
		field ("flood_missed") << flood_missed;
		field ("flood_never") << flood_never;
		field ("flood_missed_reps") << flood_missed_reps;
		field ("flood_blocks_missed") << blocks_flood_missed;
		field ("late") << late;
		field ("late_reps") << late_reps;
		field ("late_5s") << late_5s;
		field ("late_by_live") << late_by_live;
		field ("late_by_bootstrap") << late_by_bootstrap;
		field ("late_by_other") << late_by_other;
		field ("late_blocks") << blocks_late;
		field ("never") << never;
		field ("bootstrap_first") << bootstrap_first;
		field ("stalled") << stalled;
		field ("arrival_p50") << percentile (arrival, 0.5);
		field ("arrival_p90") << percentile (arrival, 0.9);
		field ("arrival_p99") << percentile (arrival, 0.99);
		field ("arrival_p999") << percentile (arrival, 0.999);
		field ("arrival_max") << largest (arrival);
		field ("arrival_all_p50") << percentile (arrival_all, 0.5);
		field ("arrival_all_p90") << percentile (arrival_all, 0.9);
		field ("arrival_all_p99") << percentile (arrival_all, 0.99);
		field ("recovery_p50") << percentile (recovery, 0.5);
		field ("recovery_p90") << percentile (recovery, 0.9);
		field ("recovery_max") << largest (recovery);
		field ("confirmation_p50") << percentile (confirmation, 0.5);
		field ("confirmation_p90") << percentile (confirmation, 0.9);
		field ("confirmation_p99") << percentile (confirmation, 0.99);
		field ("confirmation_max") << largest (confirmation);
		field ("confirmation_pairs") << confirmation.size ();
		field ("confirmation_slow") << confirmation_slow;
		field ("confirmation_slow_reps") << confirmation_slow_reps;
		field ("cemented_clean_blocks") << cemented_clean.size ();
		field ("cemented_clean_p50") << percentile (cemented_clean, 0.5);
		field ("cemented_clean_p90") << percentile (cemented_clean, 0.9);
		field ("cemented_clean_p99") << percentile (cemented_clean, 0.99);
		field ("cemented_clean_slow") << count_over (cemented_clean, slow_confirmation);
		field ("cemented_late_p50") << percentile (cemented_late, 0.5);
		field ("cemented_late_p99") << percentile (cemented_late, 0.99);

		// Of the slowest hundredth of blocks to be cemented everywhere, the share at least one node had late
		std::sort (cemented_tail.rbegin (), cemented_tail.rend ());
		auto const tail_size = std::min (cemented_tail.size (), std::max<std::size_t> (1, cemented_tail.size () / 100));
		std::size_t tail_late = 0;
		for (std::size_t i = 0; i < tail_size; ++i)
		{
			tail_late += cemented_tail[i].second ? 1 : 0;
		}
		field ("tail_blocks") << tail_size;
		field ("tail_late_share") << share (tail_late, tail_size);
	}

	std::string report ()
	{
		using namespace std::chrono;
		std::ostringstream json;
		json << std::fixed << std::setprecision (3);
		json << "{";
		auto field = [&json, first = true] (std::string const & name) mutable -> std::ostringstream & {
			if (!first)
			{
				json << ",";
			}
			first = false;
			json << "\"" << name << "\":";
			return json;
		};

		field ("label") << "\"" << params.label << "\"";
		field ("seed") << params.seed;
		field ("nodes") << params.nodes;
		field ("reps") << params.reps;
		field ("bandwidth") << "\"" << params.bandwidth << "\"";
		field ("degree") << params.degree;
		field ("timescale") << params.timescale;
		field ("rate") << params.rate;
		field ("load_seconds") << params.load_seconds;
		field ("accounts") << params.accounts;
		field ("forks") << params.forks;
		field ("restarts") << params.restarts;
		field ("restart_reps") << (params.restart_reps ? "true" : "false");
		field ("partition_start") << params.partition_start;
		field ("partition_seconds") << params.partition_seconds;
		auto const & solicitation = nodes.front ()->config->network_params.solicitation;
		field ("cap_requests") << solicitation.election_vote_requests;
		field ("cap_broadcasts") << solicitation.election_block_broadcasts;
		field ("channel_messages") << solicitation.channel_messages;
		// Timings the build ran with, in live seconds, -1 where the build has no such constant
		auto const & sample_config = *nodes.front ()->config;
		auto const & sample_network = sample_config.network_params.network;
		auto const live_s = [this] (std::chrono::milliseconds value) {
			return value.count () / 1000.0 * params.timescale;
		};
		field ("shared_reps") << params.shared_reps;
		field ("base_latency") << live_s (sample_network.election_base_latency);
		field ("broadcast_delay") << live_s (sample_network.block_broadcast_delay);
		field ("block_interval") << live_s (sample_network.block_broadcast_interval);
		field ("rebroadcast_cooldown") << live_s (sample_config.block_rebroadcaster->rebroadcast_cooldown);
		auto const optional_s = [&live_s] (auto const & source) {
			double flood = -1, passive = -1, request = -1;
			if constexpr (requires { source.block_flood_interval; })
			{
				flood = live_s (source.block_flood_interval);
			}
			if constexpr (requires { source.election_passive_duration; })
			{
				passive = live_s (source.election_passive_duration);
			}
			if constexpr (requires { source.election_request_interval; })
			{
				request = live_s (source.election_request_interval);
			}
			return std::array<double, 3>{ flood, passive, request };
		};
		auto const [flood_s, passive_s, request_s] = optional_s (sample_network);
		field ("flood_interval") << flood_s;
		field ("passive_duration") << passive_s;
		field ("request_interval") << request_s;
		auto const fanout_scale = [] (auto const & rebroadcaster) -> double {
			if constexpr (requires { rebroadcaster.fanout_scale; })
			{
				return rebroadcaster.fanout_scale;
			}
			return 1.0;
		};
		auto const scale = fanout_scale (*sample_config.block_rebroadcaster);
		field ("fanout_scale") << scale;
		field ("origin") << "\"" << params.origin << "\"";
		field ("scale_bootstrap") << (params.scale_bootstrap ? "true" : "false");
		field ("fanout") << nodes.front ()->node->network.fanout (static_cast<float> (scale));

		auto const [forks_resolved, forks_split] = fork_state ();
		field ("converged") << (converged_m ? "true" : "false");
		field ("expected_cemented") << expected_cemented;
		field ("min_cemented") << (samples.empty () ? 0 : samples.back ().min_cemented);
		field ("drain_live_seconds") << live (drain_end - load_end);

		// Latency from publish to cemented, in live seconds, for blocks published while the whole network was up and connected
		// Per node over every node, and to the last node; blocks published during a disruption are reported apart
		std::vector<double> per_node;
		std::vector<double> to_all;
		std::vector<double> disturbed_to_all;
		std::size_t missing = 0;
		std::size_t disturbed_count = 0;
		for (auto const & [hash, info] : published)
		{
			double slowest = 0;
			bool everywhere = true;
			for (auto const & entry : nodes)
			{
				nano::lock_guard<nano::mutex> guard{ entry->mutex };
				auto it = entry->cemented.find (hash);
				if (it == entry->cemented.end ())
				{
					everywhere = false;
					continue;
				}
				auto const latency = live (it->second - info.time);
				if (!info.disturbed)
				{
					per_node.push_back (latency);
				}
				slowest = std::max (slowest, latency);
			}
			disturbed_count += info.disturbed ? 1 : 0;
			if (!everywhere)
			{
				++missing;
			}
			else if (info.disturbed)
			{
				disturbed_to_all.push_back (slowest);
			}
			else
			{
				to_all.push_back (slowest);
			}
		}
		field ("published") << published.size ();
		field ("published_disturbed") << disturbed_count;
		field ("not_cemented_everywhere") << missing;
		field ("latency_p50") << percentile (per_node, 0.5);
		field ("latency_p90") << percentile (per_node, 0.9);
		field ("latency_p99") << percentile (per_node, 0.99);
		field ("latency_all_p50") << percentile (to_all, 0.5);
		field ("latency_all_p90") << percentile (to_all, 0.9);
		field ("latency_all_p99") << percentile (to_all, 0.99);
		field ("latency_all_max") << (to_all.empty () ? -1 : *std::max_element (to_all.begin (), to_all.end ()));
		field ("latency_disturbed_all_p50") << percentile (disturbed_to_all, 0.5);
		field ("latency_disturbed_all_p99") << percentile (disturbed_to_all, 0.99);

		report_propagation (field);

		field ("forks_resolved") << forks_resolved;
		field ("forks_split") << forks_split;

		// Divergence between the most and least advanced node while nothing was disrupted, and the worst moment overall
		uint64_t gap_max = 0;
		uint64_t gap_max_calm = 0;
		double gap_sum = 0;
		std::size_t gap_count = 0;
		uint64_t backlog_max = 0;
		for (auto const & s : samples)
		{
			if (s.nodes_up == 0)
			{
				continue;
			}
			auto const gap = s.max_cemented - s.min_cemented;
			gap_max = std::max (gap_max, gap);
			if (!s.disturbed)
			{
				gap_max_calm = std::max (gap_max_calm, gap);
				gap_sum += static_cast<double> (gap);
				++gap_count;
			}
			backlog_max = std::max (backlog_max, s.max_blocks - std::min (s.max_blocks, s.max_cemented));
		}
		field ("cemented_gap_max") << gap_max;
		field ("cemented_gap_calm_max") << gap_max_calm;
		field ("cemented_gap_calm_mean") << (gap_count > 0 ? gap_sum / gap_count : 0);
		field ("backlog_max") << backlog_max;
		field ("partition_recovery") << (partition_recovery ? *partition_recovery : -1);
		double restart_recovery_max = -1;
		for (auto value : restart_recoveries)
		{
			restart_recovery_max = std::max (restart_recovery_max, value);
		}
		field ("restart_recovery_max") << restart_recovery_max;
		field ("unrecovered") << recovering.size ();

		// Counters summed over every node and every instance of it
		for (auto const & key : tracked_stats ())
		{
			uint64_t sum = 0;
			for (auto const & entry : nodes)
			{
				if (auto it = entry->carried.find ({ key.type, key.detail, key.dir }); it != entry->carried.end ())
				{
					sum += it->second;
				}
				if (entry->node)
				{
					sum += entry->node->stats.count (key.type, key.detail, key.dir);
				}
			}
			field (key.name) << sum;
		}
		for (auto [name, type] : { std::pair{ "elections_confirmed", nano::stat::type::active_elections_confirmed }, std::pair{ "elections_timeout", nano::stat::type::active_elections_timeout }, std::pair{ "elections_dropped", nano::stat::type::active_elections_dropped } })
		{
			uint64_t sum = 0;
			for (auto const & entry : nodes)
			{
				if (auto it = entry->carried.find ({ type, nano::stat::detail::all, nano::stat::dir::in }); it != entry->carried.end ())
				{
					sum += it->second;
				}
				if (entry->node)
				{
					sum += election_outcomes (*entry->node, type);
				}
			}
			field (name) << sum;
		}

		// Fork resolution: from publishing a pair until every node cemented the same side, in live seconds
		std::vector<double> fork_latencies;
		std::size_t forks_unresolved = 0;
		for (auto const & pair : fork_pairs)
		{
			std::optional<double> slowest;
			for (auto const & side : { pair.a->hash (), pair.b->hash () })
			{
				double side_slowest = 0;
				bool everywhere = true;
				for (auto const & entry : nodes)
				{
					nano::lock_guard<nano::mutex> guard{ entry->mutex };
					auto it = entry->cemented.find (side);
					if (it == entry->cemented.end ())
					{
						everywhere = false;
						break;
					}
					side_slowest = std::max (side_slowest, live (it->second - pair.published));
				}
				if (everywhere)
				{
					slowest = side_slowest;
				}
			}
			if (slowest)
			{
				fork_latencies.push_back (*slowest);
			}
			else
			{
				++forks_unresolved;
			}
		}
		field ("fork_latency_p50") << percentile (fork_latencies, 0.5);
		field ("fork_latency_p90") << percentile (fork_latencies, 0.9);
		field ("fork_latency_max") << (fork_latencies.empty () ? -1 : *std::max_element (fork_latencies.begin (), fork_latencies.end ()));
		field ("forks_unresolved") << forks_unresolved;

		// Inbound load on the rep nodes, the heaviest against the rest, every instance of a restarted node included
		auto const node_count = [this] (sim_node const & entry, nano::stat::type type, nano::stat::detail detail, nano::stat::dir dir) {
			uint64_t sum = 0;
			if (auto it = entry.carried.find ({ type, detail, dir }); it != entry.carried.end ())
			{
				sum += it->second;
			}
			if (entry.node)
			{
				sum += entry.node->stats.count (type, detail, dir);
			}
			return sum;
		};
		for (auto [name, detail] : { std::pair{ "publish_in", nano::stat::detail::publish }, std::pair{ "confirm_req_in", nano::stat::detail::confirm_req } })
		{
			double top3 = 0;
			double rest = 0;
			for (std::size_t i = 0; i < params.reps; ++i)
			{
				auto const value = static_cast<double> (node_count (*nodes[i], nano::stat::type::message, detail, nano::stat::dir::in));
				if (i == 0)
				{
					field (std::string{ name } + "_rep0") << value;
				}
				(i < 3 ? top3 : rest) += value;
			}
			field (std::string{ name } + "_top3_mean") << top3 / std::min<std::size_t> (3, params.reps);
			field (std::string{ name } + "_rest_mean") << (params.reps > 3 ? rest / (params.reps - 3) : 0);
		}

		// End state of every node, the lagging ones show where the network got stuck
		field ("final_nodes") << "[";
		for (std::size_t i = 0; i < nodes.size (); ++i)
		{
			auto const & entry = *nodes[i];
			json << (i > 0 ? "," : "") << "{\"name\":\"" << entry.name << "\"";
			json << ",\"minority\":" << (!minority.empty () && minority[i] ? "true" : "false");
			if (entry.node)
			{
				json << ",\"cemented\":" << entry.node->ledger.cemented_count ();
				json << ",\"blocks\":" << entry.node->ledger.block_count ();
				json << ",\"unchecked\":" << entry.node->unchecked.count ();
				json << ",\"active\":" << entry.node->active.size ();
				json << ",\"peers\":" << entry.node->network.size ();
			}
			json << "}";
		}
		json << "]";

		auto const wall = duration<double> (drain_end - load_start).count ();
		field ("wall_seconds") << wall;
		field ("cpu_cores_used") << (cpu_end - cpu_start) / wall;

		// Where the CPU went, by thread name, in cores
		std::vector<std::pair<double, std::string>> by_thread;
		for (auto const & [name, seconds] : threads_end)
		{
			auto const before = threads_start.contains (name) ? threads_start.at (name) : 0.0;
			by_thread.emplace_back ((seconds - before) / wall, name);
		}
		std::sort (by_thread.rbegin (), by_thread.rend ());
		field ("cpu_by_thread") << "{";
		for (std::size_t i = 0; i < by_thread.size () && i < 12; ++i)
		{
			json << (i > 0 ? "," : "") << "\"" << by_thread[i].second << "\":" << by_thread[i].first;
		}
		json << "}";
		json << "}";
		return json.str ();
	}
};

std::string run_sim (sim_params const & params, bool & converged)
{
	network_sim sim{ params };
	auto result = sim.run ();
	converged = sim.converged ();
	std::cout << result << std::endl;
	if (!params.out.empty ())
	{
		std::ofstream out{ params.out, std::ios::app };
		out << result << "\n";
	}
	return result;
}
}

/*
 * Parameters from the environment, for sweeps driven from outside
 */
TEST (network_sim, custom)
{
	if (!std::getenv ("NANO_TEST_SIM"))
	{
		GTEST_SKIP () << "set NANO_TEST_SIM=1 and NANO_TEST_SIM_* parameters to run";
	}
	bool converged = false;
	run_sim (sim_params::from_env ({}), converged);
	ASSERT_TRUE (converged);
}

/*
 * Runs every line of network_sim_batch.txt in the application path, for sweeps that cannot pass an environment
 */
TEST (network_sim, batch)
{
	auto const path = nano::app_path () / "network_sim_batch.txt";
	std::ifstream batch{ path };
	if (!batch)
	{
		GTEST_SKIP () << "no " << path;
	}
	std::size_t failed = 0;
	std::string line;
	while (std::getline (batch, line))
	{
		if (line.empty () || line[0] == '#')
		{
			continue;
		}
		auto params = sim_params::from_line ({}, line);
		if (params.backend != nano::to_string (nano::default_database_backend ()))
		{
			continue;
		}
		if (params.out.empty ())
		{
			params.out = (nano::app_path () / "network_sim_results.jsonl").string ();
		}
		bool converged = false;
		run_sim (params, converged);
		failed += converged ? 0 : 1;
	}
	ASSERT_EQ (0, failed);
}

/*
 * Steady load on a well connected network with fast links: every block should be confirmed everywhere
 */
TEST (network_sim, steady)
{
	sim_params params;
	params.label = "steady";
	params.load_seconds = 120;
	params = sim_params::from_env (params);
	bool converged = false;
	run_sim (params, converged);
	ASSERT_TRUE (converged);
}

/*
 * Mixed link speeds under load with forks, a partition that heals and restarted nodes: the network should converge after the load
 */
TEST (network_sim, disrupted)
{
	sim_params params;
	params.label = "disrupted";
	params.bandwidth = "mixed";
	params.load_seconds = 300;
	params.forks = 10;
	params.restarts = 3;
	params.partition_start = 100;
	params.partition_seconds = 60;
	params = sim_params::from_env (params);
	bool converged = false;
	run_sim (params, converged);
	ASSERT_TRUE (converged);
}
