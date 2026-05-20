#include <nano/lib/enum_util.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/node/bootstrap/common.hpp>

namespace nano::bootstrap
{
nano::stat::detail to_stat_detail (nano::bootstrap::query_type type)
{
	return nano::enum_convert<nano::stat::detail> (type);
}

nano::stat::detail to_stat_detail (nano::bootstrap::query_source source)
{
	return nano::enum_convert<nano::stat::detail> (source);
}

nano::stat::type to_inspect_stat_type (nano::bootstrap::query_source source)
{
	switch (source)
	{
		case query_source::priority:
			return nano::stat::type::bootstrap_inspect_priority;
		case query_source::database:
			return nano::stat::type::bootstrap_inspect_database;
		case query_source::dependencies:
			return nano::stat::type::bootstrap_inspect_dependencies;
		case query_source::frontiers:
			return nano::stat::type::bootstrap_inspect_frontiers;
		case query_source::topology_index:
		case query_source::topology_blocks:
			return nano::stat::type::bootstrap_inspect_topo;
		case query_source::invalid:
			return nano::stat::type::bootstrap_inspect_other;
	}
	debug_assert (false);
	return nano::stat::type::bootstrap_inspect;
}
}
