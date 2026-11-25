#include <nano/node/distributed_work.hpp>
#include <nano/node/distributed_work_factory.hpp>
#include <nano/node/node.hpp>

#include <algorithm>

nano::distributed_work_factory::distributed_work_factory (nano::node & node_a) :
	node (node_a)
{
}

nano::distributed_work_factory::~distributed_work_factory ()
{
	stop ();
}

bool nano::distributed_work_factory::make (nano::work_version const version_a, nano::root const & root_a, std::vector<std::pair<std::string, uint16_t>> const & peers_a, uint64_t difficulty_a, std::function<void (std::optional<uint64_t>)> const & callback_a, std::optional<nano::account> const & account_a)
{
	return make (std::chrono::seconds (1), nano::work_request{ version_a, root_a, difficulty_a, account_a, callback_a, peers_a });
}

bool nano::distributed_work_factory::make (std::chrono::seconds const & backoff_a, nano::work_request const & request_a)
{
	if (!node.work_generation_enabled (request_a.peers))
	{
		return true;
	}
	cleanup_finished ();
	std::shared_ptr<nano::distributed_work> distributed;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		if (stopped)
		{
			return true;
		}
		distributed = std::make_shared<nano::distributed_work> (node, request_a, backoff_a);
		items.emplace (request_a.root, distributed);
	}
	distributed->start ();
	return false;
}

void nano::distributed_work_factory::retry (std::shared_ptr<nano::distributed_work> const & previous, std::chrono::seconds delay, nano::work_request const & request)
{
	std::shared_ptr<nano::distributed_work> next;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		auto [first, last] = items.equal_range (request.root);
		auto it = std::find_if (first, last, [&previous] (auto const & entry) { return entry.second.lock () == previous; });
		if (!stopped && it != last)
		{
			// Replace this attempt in place so a cancel can still reach the request during backoff.
			auto next_backoff = std::min (delay * 2, std::chrono::seconds{ 5 * 60 });
			next = std::make_shared<nano::distributed_work> (node, request, next_backoff);
			it->second = next;
		}
	}
	if (next)
	{
		next->start_after (delay);
	}
	else if (request.callback)
	{
		// The previous attempt finished as its registration was cancelled or the factory stopped.
		request.callback (std::nullopt);
	}
}

void nano::distributed_work_factory::cancel (nano::root const & root_a)
{
	std::vector<std::shared_ptr<nano::distributed_work>> cancelled;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		auto [first, last] = items.equal_range (root_a);
		for (auto it = first; it != last; ++it)
		{
			if (auto work = it->second.lock ())
			{
				cancelled.push_back (std::move (work));
			}
		}
		items.erase (first, last);
	}
	for (auto const & work : cancelled)
	{
		work->cancel ();
	}
}

void nano::distributed_work_factory::cleanup_finished ()
{
	nano::lock_guard<nano::mutex> guard (mutex);
	std::erase_if (items, [] (decltype (items)::value_type item) { return item.second.expired (); });
}

void nano::distributed_work_factory::stop ()
{
	decltype (items) cancelled;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		stopped = true;
		cancelled.swap (items);
	}
	for (auto const & [root, waiting] : cancelled)
	{
		if (auto work = waiting.lock ())
		{
			work->cancel ();
		}
	}
}

std::size_t nano::distributed_work_factory::size () const
{
	nano::lock_guard<nano::mutex> guard_l (mutex);
	return items.size ();
}

nano::container_info nano::distributed_work_factory::container_info () const
{
	nano::container_info info;
	info.put ("items", size ());
	return info;
}
