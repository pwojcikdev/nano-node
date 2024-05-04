#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/transport.hpp>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/format.hpp>

nano::transport::channel::channel (nano::node & node_a) :
	node{ node_a }
{
	set_network_version (node_a.network_params.network.protocol_version);
}

void nano::transport::channel::send (nano::message const & message, std::function<void (boost::system::error_code const &, std::size_t)> const & callback, nano::transport::buffer_drop_policy drop_policy, nano::transport::traffic_type traffic_type)
{
	auto buffer = message.to_shared_const_buffer ();
	bool sent = send_buffer (buffer, callback, drop_policy, traffic_type);
	node.stats.inc (sent ? nano::stat::type::message : nano::stat::type::drop, to_stat_detail (message.type ()), nano::stat::dir::out, /* aggregate all */ true);
}

void nano::transport::channel::set_peering_endpoint (nano::endpoint endpoint)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	peering_endpoint = endpoint;
}

nano::endpoint nano::transport::channel::get_peering_endpoint () const
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		if (peering_endpoint)
		{
			return *peering_endpoint;
		}
	}
	return get_remote_endpoint ();
}

std::shared_ptr<nano::node> nano::transport::channel::owner () const
{
	return node.shared ();
}

void nano::transport::channel::operator() (nano::object_stream & obs) const
{
	obs.write ("remote_endpoint", get_remote_endpoint ());
	obs.write ("local_endpoint", get_local_endpoint ());
	obs.write ("peering_endpoint", get_peering_endpoint ());
	obs.write ("node_id", get_node_id ().to_node_id ());
}
