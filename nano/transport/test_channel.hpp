#pragma once

#include <nano/lib/observer_set.hpp>
#include <nano/transport/channel.hpp>
#include <nano/transport/fwd.hpp>

#include <atomic>
#include <functional>
#include <future>

namespace nano::transport
{
class test_channel final : public channel
{
public:
	using channel::channel;

	/// Register a callback invoked for each sent message
	void observe (std::function<void (nano::messages::message const &, traffic_type)> observer)
	{
		observers.add (std::move (observer));
	}

	/// Returns a future that resolves when the next message of type MessageT is sent
	template <typename MessageT>
	std::future<MessageT> observe ()
	{
		auto promise = std::make_shared<std::promise<MessageT>> ();
		auto future = promise->get_future ();
		auto fulfilled = std::make_shared<std::atomic<bool>> (false);

		observers.add ([promise, fulfilled] (nano::messages::message const & message, traffic_type) {
			if (auto * casted = dynamic_cast<MessageT const *> (&message))
			{
				if (!fulfilled->exchange (true))
				{
					promise->set_value (*casted);
				}
			}
		});

		return future;
	}

	/// Register a callback invoked for each sent message of type MessageT
	template <typename MessageT>
	void observe (std::function<void (MessageT const &)> observer)
	{
		observers.add ([observer] (nano::messages::message const & message, traffic_type) {
			if (auto * casted = dynamic_cast<MessageT const *> (&message))
			{
				observer (*casted);
			}
		});
	}

	nano::endpoint get_remote_endpoint () const override
	{
		return {};
	}

	nano::endpoint get_local_endpoint () const override
	{
		return {};
	}

	transport_type get_type () const override
	{
		return transport_type::loopback;
	}

	void close () override
	{
		// Can't be closed
	}

	std::string to_string () const override
	{
		return "test_channel";
	}

protected:
	bool send_impl (nano::messages::message const & message, traffic_type traffic_type, callback_t callback) override
	{
		observers.notify (message, traffic_type);

		if (callback)
		{
			callback (boost::system::errc::make_error_code (boost::system::errc::success), message.to_shared_const_buffer ().size ());
		}

		return true;
	}

private:
	nano::observer_set<nano::messages::message, traffic_type> observers;
};
}
