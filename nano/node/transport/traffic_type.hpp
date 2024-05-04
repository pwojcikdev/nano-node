#pragma once

#include <string_view>
#include <vector>

namespace nano::transport
{
/**
 * Used for message prioritization and bandwidth limits
 */
enum class traffic_type
{
	generic,
	bootstrap, // Ascending bootstrap (asc_pull_ack, asc_pull_req) traffic
};

std::string_view to_string (traffic_type);
std::vector<traffic_type> all_traffic_types ();
}