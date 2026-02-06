#include <nano/transport/loopback.hpp>

#include <boost/format.hpp>

#include <utility>

nano::transport::loopback_channel::loopback_channel (nano::stats & stats_a, uint8_t protocol_version, nano::endpoint const & endpoint_a, nano::account const & node_id, inbound_callback inbound_a, callback_post post_callback_a) :
	transport::channel{ stats_a, protocol_version },
	inbound{ std::move (inbound_a) },
	post_callback{ std::move (post_callback_a) },
	endpoint{ endpoint_a }
{
	set_node_id (node_id);
}

bool nano::transport::loopback_channel::send_impl (nano::messages::message const & message, nano::transport::traffic_type traffic_type, nano::transport::channel::callback_t callback)
{
	stats.inc (nano::stat::type::message_loopback, to_stat_detail (message.type ()), nano::stat::dir::in);

	if (inbound)
	{
		inbound (message, shared_from_this ());
	}

	if (callback)
	{
		if (post_callback)
		{
			post_callback (std::move (callback));
		}
		else
		{
			callback (boost::system::errc::make_error_code (boost::system::errc::success), 0);
		}
	}

	return true;
}

std::string nano::transport::loopback_channel::to_string () const
{
	return boost::str (boost::format ("%1%") % endpoint);
}
