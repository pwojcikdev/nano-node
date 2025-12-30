#include <nano/lib/enum_util.hpp>
#include <nano/lib/logging_enums.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/messages/message_type.hpp>

std::string_view nano::messages::to_string (nano::messages::message_type type)
{
	return nano::enum_util::name (type);
}

nano::stat::detail nano::messages::to_stat_detail (nano::messages::message_type type)
{
	return nano::enum_util::cast<nano::stat::detail> (type);
}

nano::log::detail nano::messages::to_log_detail (nano::messages::message_type type)
{
	return nano::enum_util::cast<nano::log::detail> (type);
}
