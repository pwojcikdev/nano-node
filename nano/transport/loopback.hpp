#pragma once

#include <nano/transport/channel.hpp>
#include <nano/transport/transport.hpp>

namespace nano::transport
{
class loopback_channel final : public channel, public std::enable_shared_from_this<loopback_channel>
{
public:
	explicit loopback_channel (transport_context &, nano::endpoint endpoint = {}, void * owner_id = nullptr);

private:
	std::string to_string () const override;

	nano::endpoint get_remote_endpoint () const override
	{
		return endpoint;
	}

	nano::endpoint get_local_endpoint () const override
	{
		return endpoint;
	}

	transport_type get_type () const override
	{
		return transport_type::loopback;
	}

	void close () override
	{
		// Can't be closed
	}

protected:
	bool send_impl (nano::messages::message const &, traffic_type, callback_t) override;

private:
	nano::endpoint const endpoint;
};
}
