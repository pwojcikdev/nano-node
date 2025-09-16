#include <nano/lib/config.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/lib/utility.hpp>
#include <nano/store/write_queue.hpp>

#include <algorithm>

/*
 * write_guard
 */

nano::store::write_guard::write_guard (write_queue & queue, writer type, write_strategy strategy) :
	queue{ queue },
	type{ type },
	strategy{ strategy }
{
	renew ();
}

nano::store::write_guard::write_guard (write_guard && other) noexcept :
	queue{ other.queue },
	type{ other.type },
	strategy{ other.strategy },
	owns{ other.owns },
	token{ other.token },
	shared_lock{ std::move (other.shared_lock) },
	exclusive_lock{ std::move (other.exclusive_lock) }
{
	other.owns = false;
	other.token = 0;
}

nano::store::write_guard::~write_guard ()
{
	if (owns)
	{
		release ();
	}
}

bool nano::store::write_guard::is_owned () const
{
	return owns;
}

void nano::store::write_guard::release ()
{
	if (owns)
	{
		shared_lock.reset ();
		exclusive_lock.reset ();

		if (strategy == write_strategy::pessimistic)
		{
			queue.release (token);
		}

		owns = false;
	}
}

void nano::store::write_guard::renew ()
{
	release_assert (!owns);

	// Acquire the appropriate lock based on strategy
	if (strategy == write_strategy::pessimistic)
	{
		token = queue.acquire (type);
		exclusive_lock = std::make_unique<std::unique_lock<std::shared_mutex>> (queue.db_mutex);
	}
	else
	{
		shared_lock = std::make_unique<std::shared_lock<std::shared_mutex>> (queue.db_mutex);
	}

	owns = true;
}

/*
 * write_queue
 */

nano::store::write_queue::write_queue ()
{
}

nano::store::write_guard nano::store::write_queue::wait (writer writer, write_strategy strategy)
{
	return write_guard{ *this, writer, strategy };
}

bool nano::store::write_queue::contains (writer writer) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return std::any_of (queue.cbegin (), queue.cend (), [writer] (auto const & item) {
		return item.first == writer;
	});
}

void nano::store::write_queue::pop ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	if (!queue.empty ())
	{
		queue.pop_front ();
	}
	condition.notify_all ();
}

uint64_t nano::store::write_queue::acquire (writer writer)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	auto const token = next++;

	// Add writer to the end of the queue if it's not already waiting
	queue.push_back ({ writer, token });

	// Wait until we are at the front of the queue
	condition.wait (lock, [&] () { return queue.front ().second == token; });

	return token;
}

void nano::store::write_queue::release (uint64_t token)
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		release_assert (!queue.empty ());
		release_assert (queue.front ().second == token);
		queue.pop_front ();
	}
	condition.notify_all ();
}

/*
 *
 */

std::string_view nano::store::to_string (nano::store::writer type)
{
	return nano::enum_util::name (type);
}

std::string_view nano::store::to_string (nano::store::write_strategy strategy)
{
	return nano::enum_util::name (strategy);
}

nano::stat::type nano::store::to_stat_type (nano::store::writer type)
{
	switch (type)
	{
		case nano::store::writer::block_processor:
			return nano::stat::type::writer_block_processor;
		case nano::store::writer::bounded_backlog:
			return nano::stat::type::writer_bounded_backlog;
		case nano::store::writer::confirmation_height:
			return nano::stat::type::writer_confirmation_height;
		case nano::store::writer::voting_final:
			return nano::stat::type::writer_voting_final;
		case nano::store::writer::online_weight:
			return nano::stat::type::writer_online_weight;
		case nano::store::writer::rep_weights:
			return nano::stat::type::writer_rep_weights;
		case nano::store::writer::pruning:
			return nano::stat::type::writer_pruning;
		case nano::store::writer::node:
			return nano::stat::type::writer_node;
		case nano::store::writer::generic:
			return nano::stat::type::writer_generic;
		case nano::store::writer::testing:
			return nano::stat::type::writer_testing;
	}
	debug_assert (false, "unknown writer type");
	return {};
}