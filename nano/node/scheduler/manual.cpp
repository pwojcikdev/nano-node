#include <nano/node/active_elections.hpp>
#include <nano/node/election.hpp>
#include <nano/node/node.hpp>
#include <nano/node/scheduler/manual.hpp>

nano::scheduler::manual::manual (nano::node & node) :
	node{ node }
{
}

nano::scheduler::manual::~manual ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::scheduler::manual::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::scheduler_manual);
		run ();
	} };
}

void nano::scheduler::manual::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	nano::join_or_pass (thread);
}

void nano::scheduler::manual::notify ()
{
	condition.notify_all ();
}

void nano::scheduler::manual::push (std::shared_ptr<nano::block> const & block, boost::optional<nano::uint128_t> const & previous_balance)
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		queue.push_back (std::make_tuple (block, previous_balance, nano::election_behavior::manual));
	}
	condition.notify_all ();
}

bool nano::scheduler::manual::contains (nano::block_hash const & hash) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return std::any_of (queue.cbegin (), queue.cend (), [&hash] (auto const & item) {
		return std::get<0> (item)->hash () == hash;
	});
}

bool nano::scheduler::manual::predicate () const
{
	return !queue.empty ();
}

void nano::scheduler::manual::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [this] () {
			return stopped || predicate ();
		});

		if (stopped)
		{
			return;
		}

		debug_assert ((std::this_thread::yield (), true));

		if (predicate ())
		{
			node.stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::loop);

			auto const [block, previous_balance, election_behavior] = queue.front ();
			queue.pop_front ();
			lock.unlock ();

			auto result = node.active.insert (block, election_behavior);
			if (result.inserted)
			{
				node.stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::insert_manual);
			}
			if (result.election != nullptr)
			{
				result.election->transition_active ();
			}

			lock.lock ();
		}
	}
}

nano::container_info nano::scheduler::manual::container_info () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	nano::container_info info;
	info.put ("queue", queue);
	return info;
}