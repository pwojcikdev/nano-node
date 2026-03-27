#pragma once

#include <nano/node/fair_queue.hpp>
#include <nano/node/transport/channel.hpp>

namespace nano
{
template <>
struct fair_queue_traits<std::shared_ptr<nano::transport::channel>>
{
	static bool alive (std::shared_ptr<nano::transport::channel> const & channel)
	{
		return !channel || channel->alive ();
	}
};
}
