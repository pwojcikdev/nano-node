#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/bootstrap/priority_strategy.hpp>
#include <nano/node/nodeconfig.hpp>

namespace nano::bootstrap
{
priority_strategy::priority_strategy (nano::bootstrap_service & service_a) :
	service{ service_a }
{
}

void priority_strategy::start ()
{
	debug_assert (!thread.joinable ());
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap);
		run ();
	});
}

void priority_strategy::stop ()
{
	nano::join_or_pass (thread);
}

void priority_strategy::run ()
{
	nano::unique_lock<nano::mutex> lock{ service.mutex };
	while (!service.stopped)
	{
		lock.unlock ();
		service.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop);
		run_one ();
		lock.lock ();
	}
}

void priority_strategy::run_one ()
{
	service.wait_block_processor ();
	auto channel = service.wait_channel ();
	if (!channel)
	{
		return;
	}
	auto [account, priority, fails] = wait_priority ();
	if (account.is_zero ())
	{
		return;
	}

	// Decide how many blocks to request
	size_t const min_pull_count = 2;
	auto pull_count = std::clamp (static_cast<size_t> (priority), min_pull_count, nano::bootstrap_server::max_blocks);

	bool sent = service.request (account, pull_count, channel, nano::bootstrap::query_source::priority);

	// Only cooldown accounts that are likely to have more blocks
	// This is to avoid requesting blocks from the same frontier multiple times, before the block processor had a chance to process them
	// Not throttling accounts that are probably up-to-date allows us to evict them from the priority set faster
	if (sent && fails == 0)
	{
		nano::lock_guard<nano::mutex> lock{ service.mutex };
		service.accounts.timestamp_set (account);
	}
}

auto priority_strategy::next_priority () -> priority_result
{
	debug_assert (!service.mutex.try_lock ());

	auto next = service.accounts.next_priority ([this] (nano::account const & account) {
		return service.count_tags (account, nano::bootstrap::query_source::priority) < 4;
	});
	if (next.account.is_zero ())
	{
		return {};
	}
	service.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_priority);
	return next;
}

auto priority_strategy::wait_priority () -> priority_result
{
	priority_result result{};
	service.wait ([this, &result] () {
		debug_assert (!service.mutex.try_lock ());
		result = next_priority ();
		if (!result.account.is_zero ())
		{
			return true;
		}
		return false;
	});
	return result;
}
}
