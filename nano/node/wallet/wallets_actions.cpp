#include <nano/lib/blocks.hpp>
#include <nano/lib/container_info.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/traffic_type.hpp>
#include <nano/node/wallet.hpp>
#include <nano/node/wallet/wallets_actions.hpp>

#include <future>

using namespace std::chrono_literals;

namespace nano::wallet
{
nano::uint128_t const wallets_actions::generate_priority = std::numeric_limits<nano::uint128_t>::max ();
nano::uint128_t const wallets_actions::high_priority = std::numeric_limits<nano::uint128_t>::max () - 1;

wallets_actions::wallets_actions (nano::wallet::wallets & wallets_a, nano::node & node_a, nano::node_config const & config_a, nano::network_params const & network_params_a, nano::network & network_a, nano::logger & logger_a) :
	wallets{ wallets_a },
	node{ node_a },
	config{ config_a },
	network_params{ network_params_a },
	network{ network_a },
	logger{ logger_a },
	workers{ config.wallet_threads, nano::thread_role::name::wallet_worker, /* auto_start */ true }
{
}

wallets_actions::~wallets_actions ()
{
	debug_assert (!thread.joinable ());
}

void wallets_actions::start ()
{
	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::wallet_actions);
		run ();
	} };
}

void wallets_actions::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
		actions.clear ();
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
	workers.stop ();
}

void wallets_actions::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		if (!actions.empty ())
		{
			auto first (actions.begin ());
			auto wallet (first->second.first);
			auto current (std::move (first->second.second));
			actions.erase (first);
			// The wallet handle is id-keyed, an action against a destroyed wallet simply no-ops
			lock.unlock ();
			observer (true);
			current (*wallet);
			observer (false);
			lock.lock ();
		}
		else
		{
			condition.wait (lock);
		}
	}
}

void wallets_actions::queue (nano::uint128_t const & priority, std::shared_ptr<wallet> const & wallet_l, std::function<void (wallet &)> action)
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		actions.emplace (priority, std::make_pair (wallet_l, std::move (action)));
	}
	condition.notify_all ();
}

void wallets_actions::queue (nano::uint128_t const & priority, nano::wallet_id const & id, std::function<void (wallet &)> action)
{
	queue (priority, std::make_shared<wallet> (wallets, id), std::move (action));
}

std::shared_ptr<nano::block> wallets_actions::receive_action (nano::wallet_id const & id, nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work, bool generate_work)
{
	auto prepared = wallets.prepare_receive (id, send_hash, representative, amount, account, work);
	auto block = prepared.block;
	if (block != nullptr)
	{
		if (action_complete (id, block, account, generate_work, prepared.details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

std::shared_ptr<nano::block> wallets_actions::change_action (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, uint64_t work, bool generate_work)
{
	auto prepared = wallets.prepare_change (id, source, representative, work);
	auto block = prepared.block;
	if (block != nullptr)
	{
		if (action_complete (id, block, source, generate_work, prepared.details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

std::shared_ptr<nano::block> wallets_actions::send_action (nano::wallet_id const & id, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	auto prepared = wallets.prepare_send (id, source, destination, amount, work, send_id);
	if (prepared.cached)
	{
		// Rebroadcast the block already recorded for this send id, it has been processed before
		network.flood_block (prepared.block, nano::transport::traffic_type::block_broadcast_initial);
		return prepared.block;
	}
	auto block = prepared.block;
	if (!prepared.error && block != nullptr)
	{
		if (action_complete (id, block, source, generate_work, prepared.details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

bool wallets_actions::action_complete (nano::wallet_id const & id, std::shared_ptr<nano::block> const & block, nano::account const & account, bool const generate_work, nano::block_details const & details)
{
	bool error{ false };
	// Unschedule any work caching for this account
	delayed_work->erase (account);
	if (block != nullptr)
	{
		auto required_difficulty{ network_params.work.threshold (block->work_version (), details) };
		if (network_params.work.difficulty (*block) < required_difficulty)
		{
			logger.info (nano::log::type::wallet, "Cached or provided work for block: {}, account {}: is invalid, regenerating...",
			block->hash (),
			account);

			debug_assert (required_difficulty <= node.max_work_generate_difficulty (block->work_version ()));
			error = !node.work_generate_blocking (*block, required_difficulty).has_value ();
		}
		if (!error)
		{
			auto result = node.process_local (block);
			error = !result || result.value () != nano::block_status::progress;
			debug_assert (error || block->sideband ().details == details);
		}
		if (!error && generate_work)
		{
			// Pregenerate work for next block based on the block just created
			work_ensure (id, account, block->hash ());
		}
	}
	return error;
}

bool wallets_actions::change_sync (nano::wallet_id const & id, nano::account const & source, nano::account const & representative)
{
	std::promise<bool> result;
	std::future<bool> future = result.get_future ();
	change_async (
	id, source, representative, [&result] (std::shared_ptr<nano::block> const & block) {
		result.set_value (block == nullptr);
	},
	true);
	return future.get ();
}

void wallets_actions::change_async (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work)
{
	queue (high_priority, id, [source, representative, action, work, generate_work] (wallet & wallet_l) {
		auto block (wallet_l.change_action (source, representative, work, generate_work));
		action (block);
	});
}

bool wallets_actions::receive_sync (nano::wallet_id const & id, std::shared_ptr<nano::block> const & block, nano::account const & representative, nano::uint128_t const & amount)
{
	std::promise<bool> result;
	std::future<bool> future = result.get_future ();
	receive_async (
	id, block->hash (), representative, amount, block->destination (), [&result] (std::shared_ptr<nano::block> const & block_l) {
		result.set_value (block_l == nullptr);
	},
	true);
	return future.get ();
}

void wallets_actions::receive_async (nano::wallet_id const & id, nano::block_hash const & hash, nano::account const & representative, nano::uint128_t const & amount, nano::account const & account, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work)
{
	queue (amount, id, [hash, representative, amount, account, action, work, generate_work] (wallet & wallet_l) {
		auto block (wallet_l.receive_action (hash, representative, amount, account, work, generate_work));
		action (block);
	});
}

nano::block_hash wallets_actions::send_sync (nano::wallet_id const & id, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount)
{
	std::promise<nano::block_hash> result;
	std::future<nano::block_hash> future = result.get_future ();
	send_async (
	id, source, destination, amount, [&result] (std::shared_ptr<nano::block> const & block) {
		result.set_value (block->hash ());
	},
	true);
	return future.get ();
}

void wallets_actions::send_async (nano::wallet_id const & id, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	queue (high_priority, id, [source, destination, amount, action, work, generate_work, send_id] (wallet & wallet_l) {
		auto block (wallet_l.send_action (source, destination, amount, work, generate_work, send_id));
		action (block);
	});
}

void wallets_actions::work_cache_blocking (nano::wallet_id const & id, nano::account const & account, nano::root const & root)
{
	if (node.work_generation_enabled ())
	{
		auto difficulty (node.default_difficulty (nano::work_version::work_1));
		auto opt_work (node.work_generate_blocking (nano::work_version::work_1, root, difficulty, account));
		if (opt_work.has_value ())
		{
			wallets.update_work (id, account, root, opt_work.value ());
		}
		else if (!node.stopped)
		{
			logger.warn (nano::log::type::wallet, "Could not precache work for root: {} due to work generation failure", root);
		}
	}
}

void wallets_actions::work_ensure (nano::wallet_id const & id, nano::account const & account, nano::root const & root)
{
	std::chrono::seconds const precache_delay = network_params.network.is_dev_network () ? 1s : 10s;

	delayed_work->operator[] (account) = root;

	workers.post_delayed (precache_delay, [this, id, account, root] {
		if (stopped)
		{
			return;
		}
		auto delayed_work_l = delayed_work.lock ();
		auto existing (delayed_work_l->find (account));
		if (existing != delayed_work_l->end () && existing->second == root)
		{
			delayed_work_l->erase (existing);
			queue (generate_priority, id, [account, root] (wallet & wallet_l) {
				wallet_l.work_cache_blocking (account, root);
			});
		}
	});
}

void wallets_actions::set_observer (std::function<void (bool)> observer_a)
{
	observer = std::move (observer_a);
}

nano::container_info wallets_actions::container_info () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	nano::container_info info;
	info.put ("actions", actions.size ());
	info.put ("delayed_work", delayed_work.lock ()->size ());
	return info;
}
}
