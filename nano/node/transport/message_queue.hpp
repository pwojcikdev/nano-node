#pragma once

#include <nano/lib/asio.hpp>
#include <nano/lib/assert.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/node/transport/traffic_type.hpp>

#include <boost/container/static_vector.hpp>

#include <functional>
#include <utility>

namespace nano::transport
{
// Barebone statically allocated ring buffer implementation
template <class T, size_t Capacity>
class static_ring_buffer
{
	static_assert (Capacity > 0, "ring buffer capacity must be > 0");

public:
	bool empty () const noexcept
	{
		return m_size == 0;
	}
	bool full () const noexcept
	{
		return m_size == Capacity;
	}
	size_t size () const noexcept
	{
		return m_size;
	}
	size_t capacity () const noexcept
	{
		return Capacity;
	}

	T & front () noexcept
	{
		debug_assert (!empty ());
		return *m_buf[m_head];
	}
	T const & front () const noexcept
	{
		debug_assert (!empty ());
		return *m_buf[m_head];
	}

	void push_back (T v) noexcept
	{
		debug_assert (!full ());
		m_buf[m_tail] = std::move (v);
		m_tail = (m_tail + 1) % Capacity;
		++m_size;
	}

	void pop_front () noexcept
	{
		debug_assert (!empty ());
		m_buf[m_head].reset ();
		m_head = (m_head + 1) % Capacity;
		--m_size;
	}

private:
	std::array<std::optional<T>, Capacity> m_buf{};
	size_t m_head{ 0 }; // index of current front
	size_t m_tail{ 0 }; // index one-past last element
	size_t m_size{ 0 }; // number of live objects
};

class message_queue final
{
public:
	constexpr static size_t max_size = 32;
	constexpr static size_t full_size = 4 * max_size;
	constexpr static size_t batch_size = 8;

	using callback_t = std::function<void (boost::system::error_code const &, std::size_t)>;
	using entry_t = std::pair<nano::shared_const_buffer, callback_t>;
	using value_t = std::pair<nano::transport::traffic_type, entry_t>;
	using batch_t = boost::container::static_vector<value_t, batch_size>;

public:
	message_queue () noexcept
	{
		clear ();
	}

	bool empty () const noexcept
	{
		return total_size == 0;
	}

	size_t size () const noexcept
	{
		return total_size;
	}

	size_t size (traffic_type type) const noexcept
	{
		return queues.at (type).second.size ();
	}

	bool max (traffic_type type) const noexcept
	{
		return size (type) >= max_size;
	}

	bool full (traffic_type type) const noexcept
	{
		return size (type) >= full_size;
	}

	void clear () noexcept
	{
		for (auto type : all_traffic_types ())
		{
			queues.at (type) = { type, {} };
		}
		current = queues.end ();
		total_size = 0;
		counter = 0;
	}

	void push (traffic_type type, entry_t entry) noexcept
	{
		debug_assert (!full (type)); // Should be checked before calling this function
		auto & queue = queues.at (type).second;
		queue.push_back (std::move (entry));
		++total_size;
	}

	value_t next () noexcept
	{
		debug_assert (!empty ()); // Should be checked before calling next

		auto should_seek = [&, this] () {
			if (current == queues.end () || current->second.empty () || counter >= priority (current->first))
			{
				return true;
			}
			return false;
		};

		auto seek_next = [&, this] () {
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
		};

		if (should_seek ())
		{
			seek_next ();
		}

		release_assert (current != queues.end ());

		auto & source = current->first;
		auto & queue = current->second;

		++counter;
		--total_size;

		release_assert (!queue.empty ());
		auto entry = std::move (queue.front ());
		queue.pop_front ();
		return { source, std::move (entry) };
	}

	batch_t next_batch () noexcept
	{
		batch_t result;
		while (!empty () && result.size () < batch_size)
		{
			result.emplace_back (next ());
		}
		return result;
	}

private:
	static size_t constexpr priority (nano::transport::traffic_type type) noexcept
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

	using queue_t = std::pair<nano::transport::traffic_type, static_ring_buffer<entry_t, full_size>>;
	using queues_t = nano::enum_array<nano::transport::traffic_type, queue_t>;
	queues_t queues{};
	queues_t::iterator current{ queues.end () };

	size_t counter{ 0 };
	size_t total_size{ 0 };
};
}
