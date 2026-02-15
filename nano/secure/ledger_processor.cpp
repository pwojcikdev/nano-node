#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/work.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_processor.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/rep_weights.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger_store.hpp>

nano::ledger_processor::ledger_processor (nano::secure::write_transaction const & transaction_a, nano::ledger & ledger_a) :
	transaction (transaction_a),
	ledger (ledger_a)
{
}

/*
 * Process a legacy send block.
 * Validates the block, then deducts balance from sender and creates a pending entry for the destination.
 */
void nano::ledger_processor::send_block (nano::send_block & block)
{
	auto const hash = block.hash ();

	// Duplicate block check (harmless)
	if (ledger.any.block_exists_or_pruned (transaction, hash))
	{
		result = nano::block_status::old;
		return;
	}

	// Previous block must exist in the ledger (harmless)
	auto previous = ledger.store.block.get (transaction, block.hashables.previous);
	if (!previous)
	{
		result = nano::block_status::gap_previous;
		return;
	}

	// Block type must be valid successor of previous block type
	if (!block.valid_predecessor (*previous))
	{
		result = nano::block_status::block_position;
		return;
	}

	// Previous block must be the current head of the account (fork detection)
	auto const account = previous->account ();
	auto const info = ledger.any.account_get (transaction, account);
	release_assert (info, "missing account info for account", account.to_account ());
	if (info->head != block.hashables.previous)
	{
		result = nano::block_status::fork;
		return;
	}

	// Signature must be valid for the account holder (malformed)
	if (validate_message (account, hash, block.signature))
	{
		result = nano::block_status::bad_signature;
		return;
	}

	// Proof of work must meet the required threshold (malformed)
	nano::block_details block_details (nano::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
	if (ledger.work.difficulty (block) < ledger.work.threshold (block.work_version (), block_details))
	{
		result = nano::block_status::insufficient_work;
		return;
	}

	debug_assert (!validate_message (account, hash, block.signature));
	release_assert (info->head == block.hashables.previous);

	// Balance must not increase in a send block (Malicious)
	if (info->balance.number () < block.hashables.balance.number ())
	{
		result = nano::block_status::negative_spend;
		return;
	}

	// All validation passed — commit the block to the ledger
	result = nano::block_status::progress;
	auto amount = info->balance.number () - block.hashables.balance.number ();
	ledger.rep_weights.sub (transaction, info->representative, amount);
	block.sideband_set (nano::block_sideband (account, 0, block.hashables.balance /* unused */, info->block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
	ledger.store.block.put (transaction, hash, block);
	nano::account_info new_info (hash, info->representative, info->open_block, block.hashables.balance, nano::seconds_since_epoch (), info->block_count + 1, nano::epoch::epoch_0);
	ledger.update_account (transaction, account, *info, new_info);
	// Create pending entry so the destination account can receive the funds
	ledger.store.pending.put (transaction, nano::pending_key (block.hashables.destination, hash), { account, amount, nano::epoch::epoch_0 });
	ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::send);
}

/*
 * Process a legacy receive block.
 * Validates the block, then claims a pending entry and adds its amount to the account balance.
 */
void nano::ledger_processor::receive_block (nano::receive_block & block)
{
	auto hash = block.hash ();
	auto existing = ledger.any.block_exists_or_pruned (transaction, hash);

	// Duplicate block check (harmless)
	if (existing)
	{
		result = nano::block_status::old;
		return;
	}

	// Previous block must exist in the ledger (harmless)
	auto previous = ledger.store.block.get (transaction, block.hashables.previous);
	if (previous == nullptr)
	{
		result = nano::block_status::gap_previous;
		return;
	}

	// Block type must be valid successor of previous block type
	if (!block.valid_predecessor (*previous))
	{
		result = nano::block_status::block_position;
		return;
	}

	// Previous block must be the current head of the account (fork detection - Malicious)
	auto account = previous->account ();
	auto info = ledger.any.account_get (transaction, account);
	release_assert (info);
	if (info->head != block.hashables.previous)
	{
		result = nano::block_status::fork;
		return;
	}

	// Signature must be valid for the account holder (malformed)
	if (validate_message (account, hash, block.signature))
	{
		result = nano::block_status::bad_signature;
		return;
	}
	debug_assert (!validate_message (account, hash, block.signature));

	// Source (send) block must exist in the ledger (harmless)
	if (!ledger.any.block_exists_or_pruned (transaction, block.hashables.source))
	{
		result = nano::block_status::gap_source;
		return;
	}

	// Block must immediately follow the account's latest block (harmless)
	if (info->head != block.hashables.previous)
	{
		result = nano::block_status::gap_previous;
		return;
	}

	// A matching pending entry must exist for this account+source pair (malformed)
	nano::pending_key key (account, block.hashables.source);
	auto pending = ledger.store.pending.get (transaction, key);
	if (!pending)
	{
		result = nano::block_status::unreceivable;
		return;
	}

	// Legacy receive blocks cannot claim sends from epoch_1+ senders (malformed)
	if (pending.value ().epoch != nano::epoch::epoch_0)
	{
		result = nano::block_status::unreceivable;
		return;
	}

	// Proof of work must meet the required threshold (malformed)
	nano::block_details block_details (nano::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
	if (ledger.work.difficulty (block) < ledger.work.threshold (block.work_version (), block_details))
	{
		result = nano::block_status::insufficient_work;
		return;
	}

	// All validation passed — commit the block to the ledger
	result = nano::block_status::progress;
	auto new_balance = info->balance.number () + pending.value ().amount.number ();
	ledger.store.pending.del (transaction, key);
	block.sideband_set (nano::block_sideband (account, 0, new_balance, info->block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
	ledger.store.block.put (transaction, hash, block);
	nano::account_info new_info (hash, info->representative, info->open_block, new_balance, nano::seconds_since_epoch (), info->block_count + 1, nano::epoch::epoch_0);
	ledger.update_account (transaction, account, *info, new_info);
	ledger.rep_weights.add (transaction, info->representative, pending.value ().amount);
	ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::receive);
}

/*
 * Process a legacy open block (first block for an account).
 * Validates the block, then creates the account and claims the pending entry.
 * Note: store.account.get() returns true when the account is NOT found (error code convention).
 */
void nano::ledger_processor::open_block (nano::open_block & block)
{
	auto hash = block.hash ();
	auto existing = ledger.any.block_exists_or_pruned (transaction, hash);

	// Duplicate block check (harmless)
	if (existing)
	{
		result = nano::block_status::old;
		return;
	}

	// Signature must be valid for the declared account (malformed)
	if (validate_message (block.hashables.account, hash, block.signature))
	{
		result = nano::block_status::bad_signature;
		return;
	}
	debug_assert (!validate_message (block.hashables.account, hash, block.signature));

	// Source (send) block must exist in the ledger (harmless)
	if (!ledger.any.block_exists_or_pruned (transaction, block.hashables.source))
	{
		result = nano::block_status::gap_source;
		return;
	}

	// Account must not already exist — account.get() returns false (success) when found (Malicious)
	nano::account_info info;
	if (!ledger.store.account.get (transaction, block.hashables.account, info))
	{
		result = nano::block_status::fork;
		return;
	}

	// A matching pending entry must exist for this account+source pair (malformed)
	nano::pending_key key (block.hashables.account, block.hashables.source);
	auto pending = ledger.store.pending.get (transaction, key);
	if (!pending)
	{
		result = nano::block_status::unreceivable;
		return;
	}

	// Cannot open the burn account (Malicious)
	if (block.hashables.account == ledger.constants.burn_account)
	{
		result = nano::block_status::opened_burn_account;
		return;
	}

	// Legacy open blocks cannot claim sends from epoch_1+ senders (malformed)
	if (pending.value ().epoch != nano::epoch::epoch_0)
	{
		result = nano::block_status::unreceivable;
		return;
	}

	// Proof of work must meet the required threshold (malformed)
	nano::block_details block_details (nano::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
	if (ledger.work.difficulty (block) < ledger.work.threshold (block.work_version (), block_details))
	{
		result = nano::block_status::insufficient_work;
		return;
	}

	// All validation passed — commit the block to the ledger
	result = nano::block_status::progress;
	ledger.store.pending.del (transaction, key);
	block.sideband_set (nano::block_sideband (block.hashables.account, 0, pending.value ().amount, 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
	ledger.store.block.put (transaction, hash, block);
	nano::account_info new_info (hash, block.representative_field ().value (), hash, pending.value ().amount.number (), nano::seconds_since_epoch (), 1, nano::epoch::epoch_0);
	ledger.update_account (transaction, block.hashables.account, info, new_info);
	ledger.rep_weights.add (transaction, block.representative_field ().value (), pending.value ().amount);
	ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::open);
}

/*
 * Process a legacy change block (changes the representative without moving funds).
 * Validates the block, then updates the account's representative and moves voting weight.
 */
void nano::ledger_processor::change_block (nano::change_block & block)
{
	auto hash = block.hash ();
	auto existing = ledger.any.block_exists_or_pruned (transaction, hash);

	// Duplicate block check (harmless)
	if (existing)
	{
		result = nano::block_status::old;
		return;
	}

	// Previous block must exist in the ledger (harmless)
	auto previous = ledger.store.block.get (transaction, block.hashables.previous);
	if (previous == nullptr)
	{
		result = nano::block_status::gap_previous;
		return;
	}

	// Block type must be valid successor of previous block type
	if (!block.valid_predecessor (*previous))
	{
		result = nano::block_status::block_position;
		return;
	}

	// Previous block must be the current head of the account (fork detection)
	auto account = previous->account ();
	auto info = ledger.any.account_get (transaction, account);
	release_assert (info);
	if (info->head != block.hashables.previous)
	{
		result = nano::block_status::fork;
		return;
	}
	release_assert (info->head == block.hashables.previous);

	// Signature must be valid for the account holder (malformed)
	if (validate_message (account, hash, block.signature))
	{
		result = nano::block_status::bad_signature;
		return;
	}

	// Proof of work must meet the required threshold (malformed)
	nano::block_details block_details (nano::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
	if (ledger.work.difficulty (block) < ledger.work.threshold (block.work_version (), block_details))
	{
		result = nano::block_status::insufficient_work;
		return;
	}
	debug_assert (!validate_message (account, hash, block.signature));

	// All validation passed — commit the block to the ledger
	result = nano::block_status::progress;
	block.sideband_set (nano::block_sideband (account, 0, info->balance, info->block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
	ledger.store.block.put (transaction, hash, block);
	// Move voting weight from old representative to new representative
	auto balance = previous->balance ();
	ledger.rep_weights.move (transaction, info->representative, block.hashables.representative, balance);
	nano::account_info new_info (hash, block.hashables.representative, info->open_block, info->balance, nano::seconds_since_epoch (), info->block_count + 1, nano::epoch::epoch_0);
	ledger.update_account (transaction, account, *info, new_info);
	ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::change);
}

/*
 * Process a state block — the universal block type that can represent send, receive, change, or epoch.
 * First determines if the block is an epoch block (link field matches a known epoch link),
 * then dispatches to the appropriate implementation.
 */
void nano::ledger_processor::state_block (nano::state_block & block)
{
	result = nano::block_status::progress;
	auto is_epoch_block = false;
	if (ledger.is_epoch_link (block.hashables.link))
	{
		// validate_epoch_block checks if the previous block exists and whether the balance
		// matches the previous balance (epoch blocks must not change balance).
		// It also sets `result` to an error status if the epoch block is malformed.
		is_epoch_block = validate_epoch_block (block);
	}

	if (result != nano::block_status::progress)
	{
		return;
	}

	if (is_epoch_block)
	{
		epoch_block_impl (block);
	}
	else
	{
		state_block_impl (block);
	}
}

/*
 * Process a regular (non-epoch) state block.
 * Determines the sub-type (send/receive/change) based on balance delta and link field,
 * validates all fields, then commits the block with appropriate side effects.
 */
void nano::ledger_processor::state_block_impl (nano::state_block & block)
{
	auto hash = block.hash ();
	auto existing = ledger.any.block_exists_or_pruned (transaction, hash);

	// Duplicate block check (unambiguous)
	if (existing)
	{
		result = nano::block_status::old;
		return;
	}

	// Signature must be valid for the declared account (unambiguous)
	if (validate_message (block.hashables.account, hash, block.signature))
	{
		result = nano::block_status::bad_signature;
		return;
	}
	debug_assert (!validate_message (block.hashables.account, hash, block.signature));

	// Cannot target the burn account (unambiguous)
	if (block.hashables.account.is_zero ())
	{
		result = nano::block_status::opened_burn_account;
		return;
	}

	nano::epoch epoch (nano::epoch::epoch_0);
	nano::epoch source_epoch (nano::epoch::epoch_0);
	nano::account_info info;
	nano::amount amount (block.hashables.balance);
	auto is_send = false;
	auto is_receive = false;

	// Determine sub-type based on whether the account already exists
	// Note: account.get() returns true (error) when the account is NOT found
	auto account_error = ledger.store.account.get (transaction, block.hashables.account, info);
	if (!account_error)
	{
		// Account already exists — this is a send, receive, or representative change
		epoch = info.epoch ();

		// Existing account must not specify a zero previous (that would mean opening again - fork)
		if (block.hashables.previous.is_zero ())
		{
			result = nano::block_status::fork;
			return;
		}

		// Previous block must exist in the ledger (unambiguous)
		if (!ledger.store.block.exists (transaction, block.hashables.previous))
		{
			result = nano::block_status::gap_previous;
			return;
		}

		// Determine sub-type from balance delta:
		//   balance decreased -> send
		//   balance same/increased with non-zero link -> receive
		//   balance same with zero link -> representative change only
		is_send = block.hashables.balance < info.balance;
		is_receive = !is_send && !block.hashables.link.is_zero ();
		amount = is_send ? (info.balance.number () - amount.number ()) : (amount.number () - info.balance.number ());

		// Previous field must match the account's current head (fork detection)
		if (block.hashables.previous != info.head)
		{
			result = nano::block_status::fork;
			return;
		}
	}
	else
	{
		// Account does not yet exist — this must be an open (first block for this account)

		// First block in an account must have zero previous (unambiguous)
		if (!block.previous ().is_zero ())
		{
			result = nano::block_status::gap_previous;
			return;
		}

		// Opening block is always a receive
		is_receive = true;

		// Link must reference a source send block (unambiguous)
		if (block.hashables.link.is_zero ())
		{
			result = nano::block_status::gap_source;
			return;
		}
	}

	// Validate receive-specific fields (source block and pending entry)
	if (!is_send)
	{
		if (!block.hashables.link.is_zero ())
		{
			// Source (send) block must exist in the ledger (harmless)
			if (!ledger.any.block_exists_or_pruned (transaction, block.hashables.link.as_block_hash ()))
			{
				result = nano::block_status::gap_source;
				return;
			}

			// A matching pending entry must exist for this account+source pair (malformed)
			nano::pending_key key (block.hashables.account, block.hashables.link.as_block_hash ());
			auto pending = ledger.store.pending.get (transaction, key);
			if (!pending)
			{
				result = nano::block_status::unreceivable;
				return;
			}

			// Declared balance delta must exactly match the pending amount
			if (amount != pending.value ().amount)
			{
				result = nano::block_status::balance_mismatch;
				return;
			}
			source_epoch = pending.value ().epoch;
			epoch = std::max (epoch, source_epoch);
		}
		else
		{
			// No link and not a send — this is a pure representative change.
			// Balance must remain the same (zero delta)
			if (!amount.is_zero ())
			{
				result = nano::block_status::balance_mismatch;
				return;
			}
		}
	}

	// Proof of work must meet the required threshold for this block sub-type (malformed)
	nano::block_details block_details (epoch, is_send, is_receive, false);
	if (ledger.work.difficulty (block) < ledger.work.threshold (block.work_version (), block_details))
	{
		result = nano::block_status::insufficient_work;
		return;
	}

	// All validation passed — commit the block to the ledger
	result = nano::block_status::progress;
	ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::state_block);
	block.sideband_set (nano::block_sideband (block.hashables.account /* unused */, 0, 0 /* unused */, info.block_count + 1, nano::seconds_since_epoch (), block_details, source_epoch));
	ledger.store.block.put (transaction, hash, block);

	// Update representative voting weights
	if (!info.head.is_zero ())
	{
		// Existing account: move weight from old rep to new rep, adjusting for balance change
		// ledger.rep_weights.representation_add_dual (transaction, info.representative, 0 - info.balance.number (), block.hashables.representative, block.hashables.balance.number ());
		ledger.rep_weights.move_add_sub (transaction, info.representative, info.balance, block.hashables.representative, block.hashables.balance);
	}
	else
	{
		// New account: simply add the full balance to the chosen representative
		ledger.rep_weights.add (transaction, block.hashables.representative, block.hashables.balance);
	}

	// Update pending entries based on sub-type
	if (is_send)
	{
		// Create a pending entry so the destination can receive the funds
		nano::pending_key key (block.hashables.link.as_account (), hash);
		nano::pending_info info (block.hashables.account, amount.number (), epoch);
		ledger.store.pending.put (transaction, key, info);
	}
	else if (!block.hashables.link.is_zero ())
	{
		// Receive: delete the pending entry that was just claimed
		ledger.store.pending.del (transaction, nano::pending_key (block.hashables.account, block.hashables.link.as_block_hash ()));
	}

	nano::account_info new_info (hash, block.hashables.representative, info.open_block.is_zero () ? hash : info.open_block, block.hashables.balance, nano::seconds_since_epoch (), info.block_count + 1, epoch);
	ledger.update_account (transaction, block.hashables.account, info, new_info);
}

/*
 * Process an epoch block (network-wide protocol upgrade marker).
 * Epoch blocks are signed by the epoch signer (not the account holder), must not change
 * balance or representative, and upgrade the account's epoch version sequentially.
 */
void nano::ledger_processor::epoch_block_impl (nano::state_block & block)
{
	auto hash = block.hash ();
	auto existing = ledger.any.block_exists_or_pruned (transaction, hash);

	// Duplicate block check (unambiguous)
	if (existing)
	{
		result = nano::block_status::old;
		return;
	}

	// Epoch blocks must be signed by the epoch signer, not the account holder (unambiguous)
	if (validate_message (ledger.epoch_signer (block.hashables.link), hash, block.signature))
	{
		result = nano::block_status::bad_signature;
		return;
	}
	debug_assert (!validate_message (ledger.epoch_signer (block.hashables.link), hash, block.signature));

	// Cannot target the burn account (unambiguous)
	if (block.hashables.account.is_zero ())
	{
		result = nano::block_status::opened_burn_account;
		return;
	}

	// Validate account-specific fields depending on whether the account already exists
	// Note: account.get() returns true (error) when the account is NOT found
	nano::account_info info;
	auto account_error = ledger.store.account.get (transaction, block.hashables.account, info);
	if (!account_error)
	{
		// Account already exists — validate chain continuity

		// Existing account must not specify a zero previous (that would mean opening again - fork)
		if (block.hashables.previous.is_zero ())
		{
			result = nano::block_status::fork;
			return;
		}

		// Previous field must match the account's current head (fork detection)
		if (block.hashables.previous != info.head)
		{
			result = nano::block_status::fork;
			return;
		}

		// Epoch blocks must not change the representative
		if (block.hashables.representative != info.representative)
		{
			result = nano::block_status::representative_mismatch;
			return;
		}
	}
	else
	{
		// Account does not yet exist — epoch is opening this account

		// Representative must be zero for an epoch-opened account (no rep change allowed)
		if (!block.hashables.representative.is_zero ())
		{
			result = nano::block_status::representative_mismatch;
			return;
		}

		// Non-existing account must have pending entries to justify an epoch open
		bool pending_exists = ledger.any.receivable_exists (transaction, block.hashables.account);
		if (!pending_exists)
		{
			result = nano::block_status::gap_epoch_open_pending;
			return;
		}
	}

	// Epoch upgrade must be sequential (e.g., epoch_0 -> epoch_1 -> epoch_2)
	auto epoch = ledger.constants.epochs.epoch (block.hashables.link);
	auto is_valid_epoch_upgrade = account_error ? static_cast<std::underlying_type_t<nano::epoch>> (epoch) > 0 : nano::epochs::is_sequential (info.epoch (), epoch);
	if (!is_valid_epoch_upgrade)
	{
		result = nano::block_status::block_position;
		return;
	}

	// Epoch blocks must not change the account balance
	if (block.hashables.balance != info.balance)
	{
		result = nano::block_status::balance_mismatch;
		return;
	}

	// Proof of work must meet the required threshold for epoch blocks (malformed)
	nano::block_details block_details (epoch, false, false, true);
	if (ledger.work.difficulty (block) < ledger.work.threshold (block.work_version (), block_details))
	{
		result = nano::block_status::insufficient_work;
		return;
	}

	// All validation passed — commit the epoch block to the ledger
	result = nano::block_status::progress;
	ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::epoch_block);
	block.sideband_set (nano::block_sideband (block.hashables.account /* unused */, 0, 0 /* unused */, info.block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
	ledger.store.block.put (transaction, hash, block);
	nano::account_info new_info (hash, block.hashables.representative, info.open_block.is_zero () ? hash : info.open_block, info.balance, nano::seconds_since_epoch (), info.block_count + 1, epoch);
	ledger.update_account (transaction, block.hashables.account, info, new_info);
}

/*
 * Pre-validation for epoch blocks. Called from state_block() when the link field matches
 * a known epoch link. Uses a dual-return-channel design:
 * - Returns bool: true if the block is a genuine epoch block (balance == previous balance)
 * - Sets `result` as side-effect: error status if the block is malformed
 *
 * When the previous block is missing, this function also checks whether the block might
 * actually be a regular state block (send subtype) that happens to use an epoch link value.
 * If neither the account signature nor the epoch signature validates, it's a bad signature.
 */
bool nano::ledger_processor::validate_epoch_block (nano::state_block const & block)
{
	debug_assert (ledger.is_epoch_link (block.hashables.link));
	nano::amount prev_balance (0);
	if (!block.hashables.previous.is_zero ())
	{
		if (!ledger.store.block.exists (transaction, block.hashables.previous))
		{
			// Previous block not found — could be a gap, or this might not be an epoch block at all
			result = nano::block_status::gap_previous;
			// Check if this is actually a regular state block with a link that coincidentally
			// matches an epoch link (possible for send sub-type where link = destination account)
			if (validate_message (block.hashables.account, block.hash (), block.signature))
			{
				// Not signed by the account holder — check if it's signed by the epoch signer
				if (validate_message (ledger.epoch_signer (block.link_field ().value ()), block.hash (), block.signature))
				{
					// Neither signature is valid
					result = nano::block_status::bad_signature;
				}
			}
			return (block.hashables.balance == prev_balance);
		}
		prev_balance = ledger.any.block_balance (transaction, block.hashables.previous).value ();
	}
	// An epoch block must not change balance — if balance differs from previous, it's not an epoch block
	return (block.hashables.balance == prev_balance);
}
