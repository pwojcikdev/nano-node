#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/bootstrap/dependency_strategy.hpp>
#include <nano/node/nodeconfig.hpp>

namespace nano::bootstrap
{
dependency_strategy::dependency_strategy (nano::bootstrap_service & service_a) :
	service{ service_a }
{
}

void dependency_strategy::start ()
{
	debug_assert (!thread.joinable ());
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_dependency_walker);
		run ();
	});
}

void dependency_strategy::stop ()
{
	nano::join_or_pass (thread);
}

void dependency_strategy::run ()
{
	nano::unique_lock<nano::mutex> lock{ service.mutex };
	while (!service.stopped)
	{
		lock.unlock ();
		service.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_dependencies);
		run_one ();
		lock.lock ();
	}
}

void dependency_strategy::run_one ()
{
	// No need to wait for block_processor, as we are not processing blocks
	auto channel = service.wait_channel ();
	if (!channel)
	{
		return;
	}
	auto blocking = wait_blocking ();
	if (blocking.is_zero ())
	{
		return;
	}
	request_info (blocking, channel);
}

nano::block_hash dependency_strategy::next_blocking ()
{
	debug_assert (!service.mutex.try_lock ());

	auto blocking = service.accounts.next_blocking ([this] (nano::block_hash const & hash) {
		return service.count_tags (hash, nano::bootstrap::query_source::dependencies) == 0;
	});
	if (blocking.is_zero ())
	{
		return { 0 };
	}
	service.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_blocking);
	return blocking;
}

nano::block_hash dependency_strategy::wait_blocking ()
{
	nano::block_hash result{ 0 };
	service.wait ([this, &result] () {
		debug_assert (!service.mutex.try_lock ());
		result = next_blocking ();
		if (!result.is_zero ())
		{
			return true;
		}
		return false;
	});
	return result;
}

bool dependency_strategy::request_info (nano::block_hash hash, std::shared_ptr<nano::transport::channel> const & channel)
{
	nano::bootstrap_service::async_tag tag{};
	tag.type = nano::bootstrap::query_type::account_info_by_hash;
	tag.source = nano::bootstrap::query_source::dependencies;
	tag.hash = hash;

	nano::bootstrap_service::dependency_tag_payload payload{};
	payload.start = hash;
	tag.payload = payload;

	// Build the message
	nano::messages::asc_pull_req message{ service.network_constants };
	message.id = tag.id;
	message.type = nano::messages::asc_pull_type::account_info;

	nano::messages::asc_pull_req::account_info_payload msg_pld;
	msg_pld.target_type = nano::messages::asc_pull_req::hash_type::block; // Query account info by block hash
	msg_pld.target = hash;
	message.payload = msg_pld;
	message.update_header ();

	service.logger.debug (nano::log::type::bootstrap, "Requesting account info for: {} from: {}", hash, channel->to_string ());

	return service.send (channel, std::move (message), tag);
}

bool dependency_strategy::process (nano::messages::asc_pull_ack::account_info_payload const & response, nano::bootstrap_service::async_tag const & tag)
{
	debug_assert (!service.mutex.try_lock ());
	debug_assert (tag.type == nano::bootstrap::query_type::account_info_by_hash);
	debug_assert (!tag.hash.is_zero ());

	if (response.account.is_zero ())
	{
		service.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::account_info_empty);
		return true; // OK, but nothing to do
	}

	service.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::account_info);

	// Prioritize account containing the dependency
	service.accounts.dependency_update (tag.hash, response.account);
	service.accounts.priority_set (response.account, nano::bootstrap::account_sets_index::priority_cutoff); // Use the lowest possible priority here

	return true; // OK, no way to verify the response
}
}
