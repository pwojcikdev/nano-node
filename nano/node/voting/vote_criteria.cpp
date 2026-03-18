#include <nano/lib/blocks.hpp>
#include <nano/node/voting/vote_criteria.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/final_vote.hpp>

bool nano::vote_criteria::should_vote (nano::secure::read_transaction const & transaction, nano::ledger & ledger, nano::root const & root, nano::block_hash const & hash)
{
	auto block = ledger.any.block_get (transaction, hash);
	return block != nullptr && ledger.dependencies_cemented (transaction, *block);
}

bool nano::vote_criteria::should_vote_final (nano::secure::write_transaction const & transaction, nano::ledger & ledger, nano::root const & root, nano::block_hash const & hash)
{
	auto block = ledger.any.block_get (transaction, hash);
	return block != nullptr && ledger.dependencies_cemented (transaction, *block) && ledger.store.final_vote.put (transaction, block->qualified_root (), hash);
}

bool nano::vote_criteria::is_final (nano::secure::transaction const & transaction, nano::ledger & ledger, nano::qualified_root const & qualified_root, nano::block_hash const & hash)
{
	if (auto final_hash = ledger.store.final_vote.get (transaction, qualified_root))
	{
		return final_hash == hash;
	}
	return ledger.cemented.block_exists (transaction, hash);
}
