#include <nano/lib/object_stream.hpp>
#include <nano/lib/stacktrace.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp_channel.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/network_params.hpp>

/*
 * tcp_channel
 */

nano::transport::tcp_channel::tcp_channel (nano::node & node_a, std::shared_ptr<nano::transport::tcp_socket> socket_a, nano::transport::peer_info const & peer) :
	channel{ node_a, peer },
	socket{ socket_a },
	strand{ node_a.io_ctx.get_executor () },
	sending_task{ strand }
{
	remote_endpoint = socket_a->get_remote_endpoint ();
	local_endpoint = socket_a->get_local_endpoint ();
	start ();
}

nano::transport::tcp_channel::~tcp_channel ()
{
	close ();
}

void nano::transport::tcp_channel::close ()
{
	stop ();
	socket->close ();
}

void nano::transport::tcp_channel::close_async ()
{
	socket->close_async ();
}

void nano::transport::tcp_channel::start ()
{
	sending_task = nano::async::task (strand, [this] (nano::async::condition & condition) {
		return start_sending (condition); // This is not a coroutine, but a corotuine factory
	});
}

asio::awaitable<void> nano::transport::tcp_channel::start_sending (nano::async::condition & condition)
{
	debug_assert (strand.running_in_this_thread ());
	std::optional<tcp_channel_queue::value_t> current;
	try
	{
		co_await run_sending (condition, current);
	}
	catch (boost::system::system_error const & ex)
	{
		// Operation aborted is expected when cancelling the task
		debug_assert (ex.code () == asio::error::operation_aborted);
	}
	catch (...)
	{
		release_assert (false, "unexpected exception");
	}
	debug_assert (strand.running_in_this_thread ());

	// Stop accepting sends before completing any outstanding callbacks.
	std::deque<callback_t> callbacks;
	if (current && current->second.second)
	{
		callbacks.emplace_back (std::move (current->second.second));
	}
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
		while (auto item = queue.next ([] (traffic_type, size_t) { return true; }))
		{
			if (item->second.second)
			{
				callbacks.emplace_back (std::move (item->second.second));
			}
		}
	}
	close_async ();
	for (auto const & callback : callbacks)
	{
		callback (asio::error::operation_aborted, 0);
	}
}

void nano::transport::tcp_channel::stop ()
{
	if (sending_task.running ())
	{
		sending_task.cancel ();
		sending_task.join ();
	}
}

bool nano::transport::tcp_channel::max_impl (nano::transport::traffic_type traffic_type)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return stopped || !alive () || queue.max (traffic_type);
}

bool nano::transport::tcp_channel::send_buffer_impl (nano::shared_const_buffer const & buffer, nano::transport::traffic_type type, nano::transport::channel::callback_t callback)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	if (!stopped && alive () && !queue.full (type))
	{
		queue.push (type, { buffer, callback });
		lock.unlock ();

		node.stats.inc (nano::stat::type::tcp_channel, nano::stat::detail::queued, nano::stat::dir::out);
		node.stats.inc (nano::stat::type::tcp_channel_queued, to_stat_detail (type), nano::stat::dir::out);

		sending_task.notify (); // Wake up the sending task

		return true;
	}
	else
	{
		lock.unlock ();

		node.stats.inc (nano::stat::type::tcp_channel, nano::stat::detail::drop, nano::stat::dir::out);
		node.stats.inc (nano::stat::type::tcp_channel_drop, to_stat_detail (type), nano::stat::dir::out);
	}
	return false;
}

asio::awaitable<void> nano::transport::tcp_channel::run_sending (nano::async::condition & condition, std::optional<tcp_channel_queue::value_t> & current)
{
	while (!co_await nano::async::cancelled () && alive ())
	{
		debug_assert (strand.running_in_this_thread ());

		std::optional<std::chrono::milliseconds> retry_after;
		{
			nano::lock_guard<nano::mutex> lock{ mutex };
			current = queue.next ([this, &retry_after] (traffic_type type, size_t size) {
				auto result = node.outbound_limiter.consume (size, type);
				if (!result)
				{
					retry_after = std::min (retry_after.value_or (std::chrono::milliseconds{ 100 }), result.retry_after);
				}
				return result.granted;
			});
		}
		if (current)
		{
			auto const & [type, item] = *current;
			auto ec = co_await send_one (type, item);
			current.reset ();
			if (ec)
			{
				co_return;
			}
		}
		else if (retry_after)
		{
			node.stats.inc (nano::stat::type::tcp_channel_wait, nano::stat::detail::wait_bandwidth, nano::stat::dir::out);
			co_await condition.wait_for (*retry_after);
		}
		else
		{
			co_await condition.wait ();
		}
	}
}

asio::awaitable<boost::system::error_code> nano::transport::tcp_channel::send_one (traffic_type type, tcp_channel_queue::entry_t const & item)
{
	debug_assert (strand.running_in_this_thread ());

	auto const & [buffer, callback] = item;
	auto const size = buffer.size ();

	node.stats.inc (nano::stat::type::tcp_channel, nano::stat::detail::send, nano::stat::dir::out);
	node.stats.inc (nano::stat::type::tcp_channel_send, to_stat_detail (type), nano::stat::dir::out);

	auto [ec, size_written] = co_await socket->co_write (buffer, buffer.size ());
	debug_assert (ec || size_written == size);
	debug_assert (strand.running_in_this_thread ());

	if (!ec)
	{
		node.stats.add (nano::stat::type::traffic_tcp_type, to_stat_detail (type), nano::stat::dir::out, size_written);
		set_last_packet_sent (std::chrono::steady_clock::now ());
	}
	else
	{
		node.stats.inc (nano::stat::type::tcp_channel_ec, nano::to_stat_detail (ec), nano::stat::dir::out);
	}

	if (callback)
	{
		callback (ec, size_written);
	}

	co_return ec;
}

bool nano::transport::tcp_channel::alive () const
{
	return socket->alive ();
}

nano::endpoint nano::transport::tcp_channel::get_remote_endpoint () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return remote_endpoint;
}

nano::endpoint nano::transport::tcp_channel::get_local_endpoint () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return local_endpoint;
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
	return total_size == 0;
}

size_t nano::transport::tcp_channel_queue::size () const
{
	return total_size;
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
	return size (type) >= full_size;
}

void nano::transport::tcp_channel_queue::push (traffic_type type, entry_t entry)
{
	debug_assert (!full (type)); // Should be checked before calling this function
	queues.at (type).second.push_back (std::move (entry));
	++total_size;
}

auto nano::transport::tcp_channel_queue::next (permit_t const & permit) -> std::optional<value_t>
{
	if (empty ())
	{
		return std::nullopt;
	}
	auto advance = [this] () {
		if (current == queues.end () || ++current == queues.end ())
		{
			current = queues.begin ();
		}
		counter = 0;
	};

	auto advance_to_next = [&] () {
		do
		{
			advance ();
		} while (current->second.empty ());
	};

	if (current == queues.end () || current->second.empty () || counter >= priority (current->first))
	{
		advance_to_next ();
	}

	auto take = [&] (queue_t & source) -> std::optional<value_t> {
		auto & [type, queue] = source;
		if (queue.empty () || !permit (type, queue.front ().first.size ()))
		{
			return std::nullopt;
		}
		if (type == current->first)
		{
			++counter;
		}
		auto entry = std::move (queue.front ());
		queue.pop_front ();
		--total_size;
		return value_t{ type, std::move (entry) };
	};

	if (auto entry = take (*current))
	{
		return entry;
	}
	// Preserve a waiting generic class's turn while its shared bucket refills.
	// Bootstrap replies use their own bucket and can still be served independently.
	if (current->first != traffic_type::bootstrap_server)
	{
		return take (queues.at (traffic_type::bootstrap_server));
	}
	if (total_size > current->second.size ())
	{
		advance_to_next ();
		return take (*current);
	}
	return std::nullopt;
}

size_t nano::transport::tcp_channel_queue::priority (traffic_type type) const
{
	switch (type)
	{
		case traffic_type::block_broadcast:
		case traffic_type::vote_rebroadcast:
			return 1;
		default:
			return 4;
	}
}
