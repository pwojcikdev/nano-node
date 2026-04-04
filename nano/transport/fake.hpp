#pragma once

#include <nano/transport/channel.hpp>
#include <nano/transport/transport.hpp>

namespace nano::transport
{
/**
 * Fake channel that connects to nothing and allows its attributes to be manipulated. Mostly useful for unit tests.
 **/
namespace fake
{
	class channel final : public nano::transport::channel
	{
	public:
		explicit channel (transport_context &, nano::endpoint endpoint = {}, void * owner_id = nullptr);

		std::string to_string () const override;

		void set_endpoint (nano::endpoint const & endpoint_a)
		{
			endpoint = endpoint_a;
		}

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
			return transport_type::fake;
		}

		void close () override
		{
			closed = true;
		}

		bool alive () const override
		{
			return !closed;
		}

	protected:
		bool send_impl (nano::messages::message const &, traffic_type, callback_t) override;

	private:
		nano::endpoint endpoint;

		std::atomic<bool> closed{ false };
	};
} // namespace fake
}
