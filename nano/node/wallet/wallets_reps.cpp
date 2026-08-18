#include <nano/lib/container_info.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/wallet.hpp>
#include <nano/node/wallet/wallets_reps.hpp>
#include <nano/secure/ledger.hpp>

using namespace std::chrono_literals;

namespace nano::wallet
{
wallets_reps::wallets_reps (nano::wallet::wallets & wallets_a, nano::node & node_a, nano::ledger & ledger_a, nano::node_config const & config_a, nano::network_params const & network_params_a, nano::stats & stats_a, nano::logger & logger_a) :
	wallets{ wallets_a },
	node{ node_a },
	ledger{ ledger_a },
	config{ config_a },
	network_params{ network_params_a },
	stats{ stats_a },
	logger{ logger_a }
{
}

wallets_reps::~wallets_reps ()
{
	debug_assert (!thread.joinable ());
}

void wallets_reps::start ()
{
	if (!config.enable_voting)
	{
		return;
	}
	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::wallet_reps);
		run ();
	} };
}

void wallets_reps::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void wallets_reps::run ()
{
	auto delay = [this] () {
		// Representation drifts quickly on the test network but very slowly on the live network
		return network_params.network.is_dev_network ()
		? 100ms
		: (network_params.network.is_test_network ()
		? std::chrono::milliseconds (nano::test_scan_wallet_reps_delay ())
		: std::chrono::minutes (15));
	};

	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		lock.unlock ();

		stats.inc (nano::stat::type::wallet, nano::stat::detail::loop_reps);

		// Recompute local wallet representatives and refresh cached keys
		refresh ();

		lock.lock ();

		condition.wait_for (lock, delay (), [this] () {
			return stopped.load ();
		});
	}
}

void wallets_reps::refresh ()
{
	auto const half_principal_weight = node.minimum_principal_weight () / 2;

	wallet_representatives new_reps;
	std::unordered_map<nano::wallet_id, std::unordered_set<nano::account>> new_by_wallet;
	std::vector<std::pair<nano::wallet_id, nano::account>> rep_accounts;

	for (auto const & id : wallets.wallet_ids ())
	{
		std::unordered_set<nano::account> wallet_reps;
		for (auto const & account : wallets.accounts (id))
		{
			if (check_rep (new_reps, account, half_principal_weight))
			{
				wallet_reps.insert (account);
				rep_accounts.emplace_back (id, account);
			}
		}
		new_by_wallet[id] = std::move (wallet_reps);
	}

	std::vector<std::pair<nano::public_key, std::unique_ptr<nano::fan>>> new_cache;
	if (config.enable_voting)
	{
		for (auto const & [id, account] : rep_accounts)
		{
			// A single fetch reports the locked state too, so the password check and the fetch cannot disagree under a concurrent rekey
			// A watch-only account can hold representative weight but has no private key to fetch; it cannot vote and is left out of the cache
			auto prv_result = wallets.fetch_prv (id, account);
			if (prv_result)
			{
				new_cache.emplace_back (account, std::make_unique<nano::fan> (prv_result.value (), config.password_fanout));
			}
			else if (prv_result.error () == nano::error_common::wallet_locked)
			{
				static auto last_log = std::chrono::steady_clock::time_point ();
				if (last_log < std::chrono::steady_clock::now () - std::chrono::seconds (60))
				{
					last_log = std::chrono::steady_clock::now ();
					logger.warn (nano::log::type::wallet, "Representative locked inside wallet: {}", id);
				}
			}
		}
	}

	*representatives.lock () = std::move (new_reps);
	*representatives_by_wallet.lock () = std::move (new_by_wallet);
	rep_keys_cache.lock ()->swap (new_cache);
}

bool wallets_reps::check_rep (wallet_representatives & reps, nano::account const & account, nano::uint128_t const & half_principal_weight) const
{
	auto weight = ledger.weight (account);
	if (weight < config.vote_minimum.number ())
	{
		return false; // account not a representative
	}

	if (weight >= half_principal_weight)
	{
		reps.half_principal = true;
	}

	auto insert_result = reps.accounts.insert (account);
	if (!insert_result.second)
	{
		return false; // account already exists
	}

	++reps.voting;

	return true;
}

wallet_representatives wallets_reps::reps () const
{
	return *representatives.lock ();
}

std::unordered_set<nano::account> wallets_reps::reps (nano::wallet_id const & id) const
{
	auto locked = representatives_by_wallet.lock ();
	auto existing = locked->find (id);
	return existing != locked->end () ? existing->second : std::unordered_set<nano::account>{};
}

void wallets_reps::foreach_representative (std::function<void (nano::public_key const & pub, nano::raw_key const & prv)> const & action)
{
	if (config.enable_voting)
	{
		// Cache lock is held during callbacks, recursive calls are not allowed
		auto locked = rep_keys_cache.lock ();
		for (auto const & [pub, fan_ptr] : *locked)
		{
			nano::raw_key prv;
			fan_ptr->value (prv);
			action (pub, prv);
		}
	}
}

auto wallets_reps::signer () -> signer_t
{
	return [this] (auto const & callback) { foreach_representative (callback); };
}

nano::container_info wallets_reps::container_info () const
{
	nano::container_info info;
	info.put ("rep_keys_cache", rep_keys_cache.lock ()->size ());
	return info;
}
}
