#include <nano/boost/asio/post.hpp>
#include <nano/transport/loopback.hpp>
#include <nano/transport/ports.hpp>
#include <nano/transport/transport_context.hpp>

#include <boost/format.hpp>

nano::transport::loopback_channel::loopback_channel (transport_context & ctx_a, nano::endpoint endpoint_a, void * owner_id_a) :
	channel{ ctx_a, owner_id_a },
	endpoint{ endpoint_a }
{
	set_network_version (ctx_a.network_params.network.protocol_version);
}

bool nano::transport::loopback_channel::send_impl (nano::messages::message const & message, traffic_type traffic_type, callback_t callback)
{
	ctx.stats.inc (nano::stat::type::message_loopback, to_stat_detail (message.type ()), nano::stat::dir::in);

	ctx.message_sink->process (message, shared_from_this ());

	if (callback)
	{
		boost::asio::post (ctx.io_ctx, [callback_l = std::move (callback)] () {
			callback_l (boost::system::errc::make_error_code (boost::system::errc::success), 0);
		});
	}

	return true;
}

std::string nano::transport::loopback_channel::to_string () const
{
	return boost::str (boost::format ("%1%") % endpoint);
}
