#pragma once

#include <nano/lib/asio.hpp>
#include <nano/lib/assert.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/transport/traffic_type.hpp>

#include <deque>
#include <functional>
#include <utility>

namespace nano::transport
{
class message_queue final
{
public:
	using callback_t = std::function<void (boost::system::error_code const &, std::size_t)>;
	using entry_t = std::pair<nano::shared_const_buffer, callback_t>;
	using value_t = std::pair<traffic_type, entry_t>;
	using batch_t = std::deque<value_t>;
	using queue_t = std::pair<traffic_type, std::deque<entry_t>>;

	constexpr static size_t max_size = 32;
	constexpr static size_t full_size = 4 * max_size;
	constexpr static size_t batch_size = 8;

	message_queue ()
	{
		clear ();
	}

	void clear ()
	{
		for (auto type : all_traffic_types ())
		{
			queues.at (type) = { type, {} };
		}
		current = queues.end ();
		counter = 0;
	}

	bool empty () const
	{
		return std::all_of (queues.begin (), queues.end (), [] (auto const & queue) {
			return queue.second.empty ();
		});
	}

	size_t size () const
	{
		return std::accumulate (queues.begin (), queues.end (), size_t{ 0 }, [] (size_t acc, auto const & queue) {
			return acc + queue.second.size ();
		});
	}

	size_t size (traffic_type type) const
	{
		return queues.at (type).second.size ();
	}

	bool max (traffic_type type) const
	{
		return size (type) >= max_size;
	}

	bool full (traffic_type type) const
	{
		return size (type) >= full_size;
	}

	void push (traffic_type type, entry_t entry)
	{
		debug_assert (!full (type)); // Should be checked before calling this function
		queues.at (type).second.push_back (entry);
	}

	value_t next ()
	{
		debug_assert (!empty ());

		auto should_seek = [&, this] () {
			if (current == queues.end () || current->second.empty () || counter >= priority (current->first))
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

	batch_t next_batch (size_t max_count = batch_size)
	{
		std::deque<value_t> result;
		while (!empty () && result.size () < max_count)
		{
			result.emplace_back (next ());
		}
		return result;
	}

private:
	void seek_next ()
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

	size_t priority (traffic_type type) const
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

	nano::enum_array<traffic_type, queue_t> queues{};
	nano::enum_array<traffic_type, queue_t>::iterator current{ queues.end () };
	size_t counter{ 0 };
};
}