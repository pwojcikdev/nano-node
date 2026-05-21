#pragma once

#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/stats_enums.hpp>

namespace nano::bootstrap
{
using id_t = uint64_t;

static nano::bootstrap::id_t generate_id ()
{
	nano::bootstrap::id_t id;
	nano::random_pool::generate_block (reinterpret_cast<uint8_t *> (&id), sizeof (id));
	return id;
}

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

enum class query_source
{
	invalid = 0,
	priority,
	database,
	dependencies,
	frontiers,
	topology_index,
	topology_blocks,
};

enum class strategy
{
	priority,
	database,
	dependency,
	frontier,
	topology,
};

nano::stat::detail to_stat_detail (query_type);
nano::stat::detail to_stat_detail (query_source);
nano::stat::detail to_stat_detail (strategy);

nano::stat::type to_inspect_stat_type (query_source);
}
