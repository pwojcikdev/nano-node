#include <nano/lib/blocks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_rollback.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/rep_weights.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger/successor.hpp>

nano::ledger_rollback::ledger_rollback (nano::secure::write_transaction const & transaction_a, nano::ledger & ledger_a, std::deque<std::shared_ptr<nano::block>> & list_a, size_t depth_a, size_t max_depth_a) :
	transaction (transaction_a),
	ledger (ledger_a),
	list (list_a),
	depth (depth_a),
	max_depth (max_depth_a)
{
}

void nano::ledger_rollback::rollback (nano::stored_block const & block)
{
	std::visit ([this, &block] (auto const & inner) {
		using T = std::decay_t<decltype (inner)>;
		if constexpr (std::is_same_v<T, nano::raw_send_block>)
		{
			rollback_send (block, inner);
		}
		else if constexpr (std::is_same_v<T, nano::raw_receive_block>)
		{
			rollback_receive (block, inner);
		}
		else if constexpr (std::is_same_v<T, nano::raw_open_block>)
		{
			rollback_open (block, inner);
		}
		else if constexpr (std::is_same_v<T, nano::raw_change_block>)
		{
			rollback_change (block, inner);
		}
		else if constexpr (std::is_same_v<T, nano::raw_state_block>)
		{
			rollback_state (block, inner);
		}
	},
	block.raw ().variant ());
}

void nano::ledger_rollback::rollback_send (nano::stored_block const & stored, nano::raw_send_block const & block_a)
{
	auto hash (stored.hash ());
	nano::pending_key key (block_a.destination_field (), hash);
	auto pending = ledger.store.pending.get (transaction, key);
	while (!error && !pending.has_value ())
	{
		error = ledger.rollback (transaction, ledger.any.account_head (transaction, block_a.destination_field ()), list, depth + 1, max_depth);
		pending = ledger.store.pending.get (transaction, key);
	}
	if (!error)
	{
		auto info = ledger.any.account_get (transaction, pending.value ().source);
		release_assert (info);
		ledger.store.pending.del (transaction, key);
		ledger.rep_weights.add (transaction, info->representative, pending.value ().amount);
		nano::account_info new_info (block_a.previous_field (), info->representative, info->open_block, ledger.any.block_balance (transaction, block_a.previous_field ()).value (), nano::seconds_since_epoch (), info->block_count - 1, nano::epoch::epoch_0);
		ledger.update_account (transaction, pending.value ().source, *info, new_info);
		ledger.store.block.del (transaction, hash);
		ledger.store.successor.del (transaction, block_a.previous_field ());
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::send);
	}
}

void nano::ledger_rollback::rollback_receive (nano::stored_block const & stored, nano::raw_receive_block const & block_a)
{
	auto hash (stored.hash ());
	auto amount = ledger.any.block_amount (transaction, hash).value ().number ();
	auto destination_account = stored.account ();
	// Pending account entry can be incorrect if source block was pruned. But it's not affecting correct ledger processing
	auto source_account = ledger.any.block_account (transaction, block_a.source_field ());
	auto info = ledger.any.account_get (transaction, destination_account);
	release_assert (info);
	ledger.rep_weights.sub (transaction, info->representative, amount);
	nano::account_info new_info (block_a.previous_field (), info->representative, info->open_block, ledger.any.block_balance (transaction, block_a.previous_field ()).value (), nano::seconds_since_epoch (), info->block_count - 1, nano::epoch::epoch_0);
	ledger.update_account (transaction, destination_account, *info, new_info);
	ledger.store.block.del (transaction, hash);
	ledger.store.pending.put (transaction, nano::pending_key (destination_account, block_a.source_field ()), { source_account.value_or (0), amount, nano::epoch::epoch_0 });
	ledger.store.successor.del (transaction, block_a.previous_field ());
	ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::receive);
}

void nano::ledger_rollback::rollback_open (nano::stored_block const & stored, nano::raw_open_block const & block_a)
{
	auto hash (stored.hash ());
	auto amount = ledger.any.block_amount (transaction, hash).value ().number ();
	auto destination_account = stored.account ();
	auto source_account = ledger.any.block_account (transaction, block_a.source_field ());
	ledger.rep_weights.sub (transaction, block_a.representative_field (), amount);
	nano::account_info new_info;
	ledger.update_account (transaction, destination_account, new_info, new_info);
	ledger.store.block.del (transaction, hash);
	ledger.store.pending.put (transaction, nano::pending_key (destination_account, block_a.source_field ()), { source_account.value_or (0), amount, nano::epoch::epoch_0 });
	ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::open);
}

void nano::ledger_rollback::rollback_change (nano::stored_block const & stored, nano::raw_change_block const & block_a)
{
	auto hash (stored.hash ());
	auto rep_block_hash (ledger.representative_block (transaction, block_a.previous_field ()));
	auto account = stored.account ();
	auto info = ledger.any.account_get (transaction, account);
	release_assert (info);
	auto balance = ledger.any.block_balance (transaction, block_a.previous_field ()).value ();
	auto rep_block = ledger.any.block_get (transaction, rep_block_hash);
	release_assert (rep_block);
	auto representative = rep_block->representative_field ().value ();
	ledger.rep_weights.move (transaction, block_a.representative_field (), representative, balance);
	ledger.store.block.del (transaction, hash);
	nano::account_info new_info (block_a.previous_field (), representative, info->open_block, info->balance, nano::seconds_since_epoch (), info->block_count - 1, nano::epoch::epoch_0);
	ledger.update_account (transaction, account, *info, new_info);
	ledger.store.successor.del (transaction, block_a.previous_field ());
	ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::change);
}

void nano::ledger_rollback::rollback_state (nano::stored_block const & stored, nano::raw_state_block const & block_a)
{
	auto const hash = stored.hash ();
	auto const previous_balance = ledger.any.block_balance (transaction, block_a.previous_field ()).value_or (0).number ();
	bool const is_send = block_a.balance_field () < previous_balance;

	auto info = ledger.any.account_get (transaction, block_a.account_field ());
	release_assert (info);

	if (is_send)
	{
		nano::pending_key key (block_a.link_field ().as_account (), hash);
		while (!error && !ledger.any.pending_get (transaction, key))
		{
			error = ledger.rollback (transaction, ledger.any.account_head (transaction, block_a.link_field ().as_account ()), list, depth + 1, max_depth);
		}
		if (error)
		{
			return;
		}
		ledger.store.pending.del (transaction, key);
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::send);
	}
	else if (!block_a.link_field ().is_zero () && !ledger.is_epoch_link (block_a.link_field ()))
	{
		// Pending account entry can be incorrect if source block was pruned. But it's not affecting correct ledger processing
		auto source_account = ledger.any.block_account (transaction, block_a.link_field ().as_block_hash ());
		nano::pending_info pending_info (source_account.value_or (0), block_a.balance_field ().number () - previous_balance, stored.sideband ().source_epoch);
		ledger.store.pending.put (transaction, nano::pending_key (block_a.account_field (), block_a.link_field ().as_block_hash ()), pending_info);
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::receive);
	}

	release_assert (!error);

	nano::block_hash rep_block_hash (0);
	if (!block_a.previous_field ().is_zero ())
	{
		rep_block_hash = ledger.representative_block (transaction, block_a.previous_field ());
	}

	nano::account previous_representative{};
	if (!rep_block_hash.is_zero ())
	{
		// Move existing representation & add in amount delta
		auto rep_block = ledger.any.block_get (transaction, rep_block_hash);
		release_assert (rep_block);
		previous_representative = rep_block->representative_field ().value ();
		ledger.rep_weights.move_add_sub (transaction, block_a.representative_field (), block_a.balance_field (), previous_representative, previous_balance);
	}
	else
	{
		// Add in amount delta only
		ledger.rep_weights.sub (transaction, block_a.representative_field (), block_a.balance_field ().number ());
	}

	auto previous_version (ledger.version (transaction, block_a.previous_field ()));
	nano::account_info new_info (block_a.previous_field (), previous_representative, info->open_block, previous_balance, nano::seconds_since_epoch (), info->block_count - 1, previous_version);
	ledger.update_account (transaction, block_a.account_field (), *info, new_info);

	if (!block_a.previous_field ().is_zero ())
	{
		debug_assert (ledger.any.block_exists_or_pruned (transaction, block_a.previous_field ()));
		ledger.store.successor.del (transaction, block_a.previous_field ());
	}
	else
	{
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::open);
	}

	ledger.store.block.del (transaction, hash);
}
