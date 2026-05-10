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
	unsigned consideration_count{ 4 };
	std::size_t candidates{ 1000 };
	std::chrono::milliseconds cooldown{ 1000 * 3 };
	std::chrono::milliseconds block_retry{ 1000 * 5 };
	std::size_t block_batch_size{ 128 };
	std::size_t max_blocks_outstanding{ 10000 };
	std::size_t max_blocks_queued{ 40000 };
	// If the queue has outstanding work but nothing drains for this long, the
	// discovery state is considered poisoned (unprocessable blocks, gaps left by
	// dropped submissions, ...) and the pipeline is rewound. Acts as a fail-safe
	// when the verify / dedup / backpressure safeguards don't prevent the stall.
	std::chrono::milliseconds poisoning_timeout{ 1000 * 60 };
	// Escalating-rollback step. When a poisoning reset clears the queue but the
	// retry from `indexed` still makes no progress (gap below the anchor), the
	// indexed cursor is rewound by this many topo-heights, doubling on every
	// further unproductive reset until a workable position is found. Reset to
	// `rollback_min` once a reset cycle drains at least one block.
	uint64_t rollback_min{ 1024 };
	// Upper bound on the doubling so the step can't overflow; once the rewind
	// distance reaches/exceeds the indexed height the cursor lands at genesis.
	uint64_t rollback_max{ 4 * 1024 * 1024 };
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
	std::size_t rate_limit{ 500 };
	std::size_t database_rate_limit{ 250 };
	std::size_t frontier_rate_limit{ 8 };
	std::size_t database_warmup_ratio{ 10 };
	std::size_t max_pull_count{ nano::bootstrap_server::max_blocks };
	std::chrono::milliseconds request_timeout{ 1000 * 15 };
	std::size_t throttle_coefficient{ 8 * 1024 };
	std::chrono::milliseconds throttle_wait{ 100 };
	std::size_t block_processor_threshold{ 1000 };
	std::size_t max_requests{ 1024 };
	unsigned optimistic_request_percentage{ 75 };

	account_sets_config account_sets;
	frontier_scan_config frontier_scan;
	topo_scan_config topo_scan;
};
}
