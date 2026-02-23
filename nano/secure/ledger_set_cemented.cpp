#include <nano/lib/blocks.hpp>
#include <nano/lib/stored_block.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/confirmation_height.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger/pruned.hpp>
#include <nano/store/ledger_store.hpp>

nano::ledger_set_cemented::ledger_set_cemented (nano::ledger const & ledger) :
	ledger{ ledger }
{
}

std::optional<nano::amount> nano::ledger_set_cemented::account_balance (secure::transaction const & transaction, nano::account const & account_a) const
{
	auto block = block_get (transaction, account_head (transaction, account_a));
	if (!block)
	{
		return std::nullopt;
	}
	return block->balance ();
}

nano::block_hash nano::ledger_set_cemented::account_head (secure::transaction const & transaction, nano::account const & account) const
{
	auto info = ledger.store.confirmation_height.get (transaction, account);
	if (!info)
	{
		return 0;
	}
	return info.value ().frontier;
}

uint64_t nano::ledger_set_cemented::account_height (secure::transaction const & transaction, nano::account const & account) const
{
	auto head_l = account_head (transaction, account);
	if (head_l.is_zero ())
	{
		return 0;
	}
	auto block = block_get (transaction, head_l);
	release_assert (block); // Head block must be in ledger
	return block->sideband ().height;
}

std::optional<nano::amount> nano::ledger_set_cemented::block_balance (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block = block_get (transaction, hash);
	if (!block)
	{
		return std::nullopt;
	}
	return block->balance ();
}

bool nano::ledger_set_cemented::block_exists (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	return block_get (transaction, hash).has_value ();
}

bool nano::ledger_set_cemented::block_exists (secure::transaction const & transaction, nano::block const & block) const
{
	auto info = ledger.store.confirmation_height.get (transaction, block.account ());
	if (!info)
	{
		return false;
	}
	return block.sideband ().height <= info.value ().height;
}

bool nano::ledger_set_cemented::block_exists (secure::transaction const & transaction, nano::stored_block const & block) const
{
	auto info = ledger.store.confirmation_height.get (transaction, block.account ());
	if (!info)
	{
		return false;
	}
	return block.sideband ().height <= info.value ().height;
}

bool nano::ledger_set_cemented::block_exists_or_pruned (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	if (hash.is_zero ())
	{
		return false;
	}
	if (ledger.store.pruned.exists (transaction, hash))
	{
		return true;
	}
	return block_exists (transaction, hash);
}

std::optional<nano::stored_block> nano::ledger_set_cemented::block_get (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	if (hash.is_zero ())
	{
		return std::nullopt;
	}
	auto block = ledger.store.block.get_stored (transaction, hash);
	if (!block)
	{
		return std::nullopt;
	}
	auto info = ledger.store.confirmation_height.get (transaction, block->account ());
	if (!info)
	{
		return std::nullopt;
	}
	return block->sideband ().height <= info.value ().height ? block : std::nullopt;
}
auto nano::ledger_set_cemented::receivable_end () const -> receivable_iterator
{
	return receivable_iterator{};
}

auto nano::ledger_set_cemented::receivable_upper_bound (secure::transaction const & transaction, nano::account const & account) const -> receivable_iterator
{
	return receivable_iterator{ transaction, *this, receivable_lower_bound (transaction, account.number () + 1, 0) };
}

auto nano::ledger_set_cemented::receivable_upper_bound (secure::transaction const & transaction, nano::account const & account, nano::block_hash const & hash) const -> receivable_iterator
{
	auto result = receivable_lower_bound (transaction, account, hash.number () + 1);
	if (!result || result.value ().first.account != account)
	{
		return receivable_iterator{ transaction, *this, std::nullopt };
	}
	return receivable_iterator{ transaction, *this, result };
}

std::optional<std::pair<nano::pending_key, nano::pending_info>> nano::ledger_set_cemented::receivable_lower_bound (secure::transaction const & transaction, nano::account const & account, nano::block_hash const & hash) const
{
	auto result = ledger.store.pending.begin (transaction, { account, hash });
	while (result != ledger.store.pending.end (transaction) && !block_exists (transaction, result->first.hash))
	{
		++result;
	}
	if (result == ledger.store.pending.end (transaction))
	{
		return std::nullopt;
	}
	return *result;
}
