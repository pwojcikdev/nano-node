#pragma once

#include <nano/lib/errors.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/bootstrap/bootstrap_server.hpp>

using namespace std::chrono_literals;

namespace nano
{
class account_sets_config final
{
public:
	nano::error deserialize (nano::tomlconfig &);
	nano::error serialize (nano::tomlconfig &) const;

public:
	std::size_t consideration_count{ 4 };
	std::size_t priorities_max{ 256 * 1024 };
	std::size_t blocking_max{ 256 * 1024 };
	std::chrono::milliseconds cooldown{ 1000 * 3 };
	std::chrono::seconds blocking_decay{ 15min };
};

class frontier_scan_config final
{
public:
	nano::error deserialize (nano::tomlconfig &);
	nano::error serialize (nano::tomlconfig &) const;

public:
	unsigned head_parallelism{ 128 };
	unsigned consideration_count{ 4 };
	std::size_t candidates{ 1000 };
	std::chrono::milliseconds cooldown{ 1000 * 5 };
	std::size_t max_pending{ 16 };
};

class topo_scan_config final
{
public:
	nano::error deserialize (nano::tomlconfig &);
	nano::error serialize (nano::tomlconfig &) const;

public:
	// Number of concurrent scanning heads: head 0 is the spear (scans the frontier
	// forward in topo order), the rest are repair heads (re-scan nested top-fraction
	// ranges below the frontier to refetch keys the spear's union skipped). Minimum 1
	// (spear only).
	unsigned head_count{ 4 };
	// Weighted scheduling ratio of the spear to the repair heads on the single scan
	// thread: the spear is served this many requests for every 1 given to a repair head
	// (round-robin across the repair heads). 4 = spear gets 4/5 of the requests, the
	// repair heads share 1/5. Minimum 1 (1:1). Raise to favour frontier progress.
	unsigned spear_weight{ 4 };
	unsigned consideration_count{ 4 };
	std::size_t candidates{ 1000 };
	std::chrono::milliseconds cooldown{ 3s };
	std::chrono::milliseconds block_retry{ 5s };
	std::size_t block_batch_size{ 128 };
	std::size_t max_blocks_outstanding{ 10'000 };
	std::size_t max_blocks_queued{ 40'000 };
	std::size_t block_processor_threshold{ 2000 };
	// Number of pending dependency gaps the spear tolerates before pausing. Below
	// this the spear keeps discovering and keeps submitting past gaps (making
	// progress on independent account chains); once the gap count reaches the
	// threshold the spear pauses and waits for the repair heads to clear every gap
	// before resuming. 1 = pause at the first gap. Clamped to a minimum of 1.
	std::size_t gap_threshold{ 1000 };
	// The repair heads stay idle until the spear frontier's topo_height reaches this
	// threshold ("kick in once the spear is established"). At the start of a bootstrap
	// the spear sits at topo_height ~1 for a long time — genesis + the very many
	// dependency-free epoch-open blocks all share that height — and those root blocks
	// can never gap, so repair is useless there and only thrashes. Gating activation on
	// the spear's height skips that phase. Each head still covers its full range
	// (head 1 down to genesis) once active. Tune to where the dense low layer ends.
	uint64_t repair_activation_height{ 100 };
};

class bootstrap_config final
{
public:
	nano::error deserialize (nano::tomlconfig &);
	nano::error serialize (nano::tomlconfig &) const;

public:
	bool enable{ true };
	bool enable_priorities{ true };
	bool enable_database_scan{ false };
	bool enable_dependency_walker{ true };
	bool enable_frontier_scan{ true };
	bool enable_topology{ true };

	// Maximum number of un-responded requests per channel, should be lower or equal to bootstrap server max queue size
	std::size_t channel_limit{ 16 };
	std::size_t max_requests{ 1000 };
	std::chrono::milliseconds request_timeout{ 1000 * 15 };

	// Global rate_limit is retained for back-compat config keys but is no longer applied; rate limiting is per-strategy
	std::size_t rate_limit{ 500 };
	std::size_t priority_rate_limit{ 500 };
	std::size_t database_rate_limit{ 250 };
	std::size_t dependency_rate_limit{ 500 };
	std::size_t frontier_rate_limit{ 15 };
	std::size_t topology_rate_limit{ 500 };

	std::size_t database_warmup_ratio{ 10 };
	std::size_t max_pull_count{ nano::bootstrap_server::max_blocks };
	std::size_t throttle_coefficient{ 8000 };
	std::chrono::milliseconds throttle_wait{ 100 };
	std::size_t block_processor_threshold{ 1000 };
	unsigned optimistic_request_percentage{ 75 };

	account_sets_config account_sets;
	frontier_scan_config frontier_scan;
	topo_scan_config topo_scan;
};
}
