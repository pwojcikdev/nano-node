#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/message_deserializer.hpp>
#include <nano/node/transport/tcp_channel.hpp>

/*
 * tcp_channel
 */

nano::transport::tcp_channel::tcp_channel (nano::node & node_a, std::shared_ptr<nano::transport::tcp_socket> socket_a) :
	channel (node_a),
	socket{ std::move (socket_a) },
	strand{ node_a.io_ctx.get_executor () },
	sending_task{ strand },
	sending_condition{ strand }
{
	release_assert (socket != nullptr);
	start ();
}

nano::transport::tcp_channel::~tcp_channel ()
{
	close ();
}

void nano::transport::tcp_channel::update_endpoints ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	debug_assert (remote_endpoint == nano::endpoint{}); // Not initialized endpoint value
	debug_assert (local_endpoint == nano::endpoint{}); // Not initialized endpoint value

	remote_endpoint = socket->remote_endpoint ();
	local_endpoint = socket->local_endpoint ();
}

void nano::transport::tcp_channel::close ()
{
	socket->close ();
	stop ();
}

void nano::transport::tcp_channel::start ()
{
	sending_task = nano::async::task (strand, [this] () -> asio::awaitable<void> {
		try
		{
			co_await run_sending ();
		}
		catch (boost::system::system_error const & ex)
		{
			// Operation aborted is expected when cancelling the acceptor
			debug_assert (ex.code () == asio::error::operation_aborted);
		}
		debug_assert (strand.running_in_this_thread ());
	});
}

void nano::transport::tcp_channel::stop ()
{
	if (sending_task.joinable ())
	{
		sending_task.cancel ();
		sending_task.join ();
	}
}

bool nano::transport::tcp_channel::max (nano::transport::traffic_type traffic_type)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return queue.max (traffic_type);
}

bool nano::transport::tcp_channel::send_buffer (nano::shared_const_buffer const & buffer, std::function<void (boost::system::error_code const &, std::size_t)> const & callback, nano::transport::buffer_drop_policy policy, nano::transport::traffic_type traffic_type)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	if (!queue.max (traffic_type) || (policy == buffer_drop_policy::no_socket_drop && !queue.full (traffic_type)))
	{
		queue.push (traffic_type, { buffer, callback });
		lock.unlock ();
		sending_condition.notify ();
		return true;
	}
	else
	{
		// TODO: Stat & log
	}
	return false;

	// if (!socket->max (traffic_type) || (policy_a == nano::transport::buffer_drop_policy::no_socket_drop && !socket->full (traffic_type)))
	// {
	// 	socket->async_write (
	// 	buffer_a, [this_s = shared_from_this (), endpoint_a = socket->remote_endpoint (), node = std::weak_ptr<nano::node>{ node.shared () }, callback_a] (boost::system::error_code const & ec, std::size_t size_a) {
	// 		if (auto node_l = node.lock ())
	// 		{
	// 			if (!ec)
	// 			{
	// 				this_s->set_last_packet_sent (std::chrono::steady_clock::now ());
	// 			}
	// 			if (ec == boost::system::errc::host_unreachable)
	// 			{
	// 				node_l->stats.inc (nano::stat::type::error, nano::stat::detail::unreachable_host, nano::stat::dir::out);
	// 			}
	// 			if (callback_a)
	// 			{
	// 				callback_a (ec, size_a);
	// 			}
	// 		}
	// 	},
	// 	traffic_type);
	// }
	// else
	// {
	// 	if (policy_a == nano::transport::buffer_drop_policy::no_socket_drop)
	// 	{
	// 		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_no_socket_drop, nano::stat::dir::out);
	// 	}
	// 	else
	// 	{
	// 		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_drop, nano::stat::dir::out);
	// 	}
	// 	if (callback_a)
	// 	{
	// 		callback_a (boost::system::errc::make_error_code (boost::system::errc::no_buffer_space), 0);
	// 	}
	// }
}

asio::awaitable<void> nano::transport::tcp_channel::run_sending ()
{
	debug_assert (strand.running_in_this_thread ());

	while (!co_await nano::async::cancelled ())
	{
		auto next_batch = [this] () {
			const size_t max_batch = 8; // TODO: Make this configurable

			nano::lock_guard<nano::mutex> lock{ mutex };
			return queue.next_batch (max_batch);
		};

		if (auto batch = next_batch (); !batch.empty ())
		{
			for (auto const & [type, item] : batch)
			{
				co_await send_one (type, item);
			}
		}
		else
		{
			co_await sending_condition.wait_for (60s);
		}
	}
}

asio::awaitable<void> nano::transport::tcp_channel::send_one (traffic_type type, tcp_channel_queue::entry_t const & item)
{
	debug_assert (strand.running_in_this_thread ());

	auto const & [buffer, callback] = item;

	co_await wait_available_socket ();
	co_await wait_avaialble_bandwidth (type, buffer.size ());

	socket->async_write (
	buffer,
	[this_s = shared_from_this (), callback] (boost::system::error_code const & ec, std::size_t size) {
		if (!ec)
		{
			this_s->set_last_packet_sent (std::chrono::steady_clock::now ());
		}
		if (ec == boost::system::errc::host_unreachable)
		{
			this_s->node.stats.inc (nano::stat::type::error, nano::stat::detail::unreachable_host, nano::stat::dir::out);
		}
		if (callback)
		{
			callback (ec, size);
		}
	});
}

asio::awaitable<void> nano::transport::tcp_channel::wait_avaialble_bandwidth (nano::transport::traffic_type traffic_type, size_t size)
{
	debug_assert (strand.running_in_this_thread ());

	if (allocated_bandwidth >= size)
	{
		allocated_bandwidth -= size;
		co_return;
	}

	const size_t bandwidth_chunk = 128 * 1024; // TODO: Make this configurable

	// This is somewhat inefficient
	// The performance impact *should* be mitigated by the fact that we allocate it in larger chunks, so this happens relatively infrequently
	// TODO: Consider implementing a subsribe/notification mechanism for bandwidth allocation
	while (!node.outbound_limiter.should_pass (bandwidth_chunk, traffic_type))
	{
		co_await nano::async::sleep_for (100ms);
	}
}

asio::awaitable<void> nano::transport::tcp_channel::wait_available_socket ()
{
	debug_assert (strand.running_in_this_thread ());

	while (socket->full ())
	{
		co_await nano::async::sleep_for (100ms);
	}
}

std::string nano::transport::tcp_channel::to_string () const
{
	return nano::util::to_str (get_remote_endpoint ());
}

void nano::transport::tcp_channel::operator() (nano::object_stream & obs) const
{
	nano::transport::channel::operator() (obs); // Write common data

	obs.write ("socket", socket);
}

/*
 * tcp_channel_queue
 */

nano::transport::tcp_channel_queue::tcp_channel_queue ()
{
	for (auto type : all_traffic_types ())
	{
		queues.at (type) = { type, {} };
	}
}

bool nano::transport::tcp_channel_queue::empty () const
{
	return std::all_of (queues.begin (), queues.end (), [] (auto const & queue) {
		return queue.second.empty ();
	});
}

size_t nano::transport::tcp_channel_queue::size () const
{
	return std::accumulate (queues.begin (), queues.end (), size_t{ 0 }, [] (size_t acc, auto const & queue) {
		return acc + queue.second.size ();
	});
}

size_t nano::transport::tcp_channel_queue::size (traffic_type type) const
{
	return queues.at (type).second.size ();
}

bool nano::transport::tcp_channel_queue::max (traffic_type type) const
{
	return size (type) >= max_size;
}

bool nano::transport::tcp_channel_queue::full (traffic_type type) const
{
	return size (type) >= max_size * 2;
}

void nano::transport::tcp_channel_queue::push (traffic_type type, entry_t entry)
{
	debug_assert (!full (type)); // Should be checked before calling this function
	queues.at (type).second.push_back (entry);
}

auto nano::transport::tcp_channel_queue::next () -> value_t
{
	debug_assert (!empty ()); // Should be checked before calling next

	auto should_seek = [&, this] () {
		if (current == queues.end ())
		{
			return true;
		}
		auto & queue = current->second;
		if (queue.empty ())
		{
			return true;
		}
		// Allow up to `priority` requests to be processed before moving to the next queue
		if (counter >= priority (current->first))
		{
			return true;
		}
		return false;
	};

	if (should_seek ())
	{
		seek_next ();
	}

	release_assert (current != queues.end ());

	auto & source = current->first;
	auto & queue = current->second;

	++counter;

	release_assert (!queue.empty ());
	auto entry = queue.front ();
	queue.pop_front ();
	return { source, entry };
}

auto nano::transport::tcp_channel_queue::next_batch (size_t max_count) -> batch_t
{
	// TODO: Naive implementation, could be optimized
	std::deque<value_t> result;
	while (!empty () && result.size () < max_count)
	{
		result.emplace_back (next ());
	}
	return result;
}

size_t nano::transport::tcp_channel_queue::priority (traffic_type type) const
{
	switch (type)
	{
		case traffic_type::generic:
			return 1;
		case traffic_type::bootstrap:
			return 1;
	}
	debug_assert (false);
	return 1;
}

void nano::transport::tcp_channel_queue::seek_next ()
{
	counter = 0;
	do
	{
		if (current != queues.end ())
		{
			++current;
		}
		if (current == queues.end ())
		{
			current = queues.begin ();
		}
		release_assert (current != queues.end ());
	} while (current->second.empty ());
}
