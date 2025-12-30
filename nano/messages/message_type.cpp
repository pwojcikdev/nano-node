#include <nano/lib/enum_util.hpp>
#include <nano/lib/logging_enums.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/messages/message_type.hpp>

std::string_view nano::to_string (nano::message_type type)
{
	return nano::enum_util::name (type);
}

nano::stat::detail nano::to_stat_detail (nano::message_type type)
{
	return nano::enum_util::cast<nano::stat::detail> (type);
}

nano::log::detail nano::to_log_detail (nano::message_type type)
{
	return nano::enum_util::cast<nano::log::detail> (type);
}
