#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/stats_enums.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace nano::bootstrap
{
using id_t = uint64_t;

id_t generate_id ();

enum class query_type
{
	invalid = 0,
	blocks_by_hash,
	blocks_by_account,
	account_info_by_hash,
	frontiers,
	blocks_random,
	topo_index,
};

enum class strategy
{
	invalid = 0,
	priority,
	database,
	dependency,
	frontier,
	topo,
};

enum class verify_result
{
	ok,
	nothing_new,
	invalid,
};

enum class peer_probe_status;

struct peer_probes
{
	std::function<peer_probe_status (std::span<nano::account const> exclude)> peer_status;
	std::function<size_t (std::span<id_t const> tag_ids)> count_inflight;
};

nano::stat::detail to_stat_detail (nano::bootstrap::query_type);
nano::stat::detail to_stat_detail (nano::bootstrap::strategy);

std::string_view to_string (nano::bootstrap::strategy);

nano::stat::type to_inspect_stat_type (strategy);
}
