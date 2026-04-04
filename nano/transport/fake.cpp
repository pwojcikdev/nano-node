#include <nano/boost/asio/post.hpp>
#include <nano/transport/fake.hpp>
#include <nano/transport/transport_context.hpp>

#include <boost/format.hpp>

nano::transport::fake::channel::channel (transport_context & ctx_a, nano::endpoint endpoint_a, void * owner_id_a) :
	nano::transport::channel{ ctx_a, owner_id_a },
	endpoint{ endpoint_a }
{
	set_network_version (ctx_a.network_params.network.protocol_version);
}

/**
 * The send function behaves like a null device, it throws the data away and returns success.
 */
bool nano::transport::fake::channel::send_impl (nano::messages::message const & message, traffic_type traffic_type, callback_t callback)
{
	auto buffer = message.to_shared_const_buffer ();
	auto size = buffer.size ();
	if (callback)
	{
		boost::asio::post (ctx.io_ctx, [callback, size] () {
			callback (boost::system::errc::make_error_code (boost::system::errc::success), size);
		});
	}
	return true;
}

std::string nano::transport::fake::channel::to_string () const
{
	return boost::str (boost::format ("%1%") % endpoint);
}
