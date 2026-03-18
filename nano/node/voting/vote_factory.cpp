#include <nano/lib/blocks.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/vote.hpp>
#include <nano/node/voting/vote_factory.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/final_vote.hpp>

/*
 * verified_candidate
 */

nano::vote_factory::verified_candidate::verified_candidate (nano::root const & root_a, nano::block_hash const & hash_a, bool is_final_a) :
	root (root_a),
	hash (hash_a),
	is_final (is_final_a)
{
}

/*
 * vote_factory
 */

nano::vote_factory::vote_factory (nano::ledger & ledger_a, nano::wallets & wallets_a) :
	ledger (ledger_a),
	wallets (wallets_a)
{
}

auto nano::vote_factory::check (nano::secure::read_transaction const & transaction, nano::root const & root, nano::block_hash const & hash) const -> std::optional<verified_candidate>
{
	auto block = ledger.any.block_get (transaction, hash);
	if (block && ledger.dependencies_cemented (transaction, *block))
	{
		return verified_candidate{ root, hash, false };
	}
	return std::nullopt;
}

auto nano::vote_factory::check_final (nano::secure::write_transaction const & transaction, nano::root const & root, nano::block_hash const & hash) const -> std::optional<verified_candidate>
{
	auto block = ledger.any.block_get (transaction, hash);
	if (block && ledger.dependencies_cemented (transaction, *block) && ledger.store.final_vote.put (transaction, block->qualified_root (), hash))
	{
		return verified_candidate{ root, hash, true };
	}
	return std::nullopt;
}

bool nano::vote_factory::is_final (nano::secure::transaction const & transaction, nano::qualified_root const & qualified_root, nano::block_hash const & hash) const
{
	if (auto final_hash = ledger.store.final_vote.get (transaction, qualified_root))
	{
		return final_hash == hash;
	}
	return ledger.cemented.block_exists (transaction, hash);
}

auto nano::vote_factory::check_block (nano::secure::transaction const & transaction, nano::block const & block) const -> std::optional<verified_candidate>
{
	if (!ledger.dependencies_cemented (transaction, block))
	{
		return std::nullopt;
	}
	bool final_vote = is_final (transaction, block.qualified_root (), block.hash ());
	return verified_candidate{ block.root (), block.hash (), final_vote };
}

auto nano::vote_factory::sign (std::vector<verified_candidate> const & candidates) const -> std::vector<std::shared_ptr<nano::vote>>
{
	if (candidates.empty ())
	{
		return {};
	}

	// All candidates in a batch must have the same is_final
	bool const batch_is_final = candidates.front ().is_final;
	debug_assert (std::all_of (candidates.begin (), candidates.end (), [batch_is_final] (auto const & c) {
		return c.is_final == batch_is_final;
	}));

	std::vector<nano::block_hash> hashes;
	hashes.reserve (candidates.size ());
	for (auto const & c : candidates)
	{
		hashes.push_back (c.hash);
	}

	std::vector<std::shared_ptr<nano::vote>> votes;
	wallets.foreach_representative ([&hashes, &votes, batch_is_final] (nano::public_key const & pub, nano::raw_key const & prv) {
		auto timestamp = batch_is_final ? nano::vote::timestamp_max : nano::milliseconds_since_epoch ();
		uint8_t duration = batch_is_final ? nano::vote::duration_max : /*8192ms*/ 0x9;
		votes.emplace_back (std::make_shared<nano::vote> (pub, prv, timestamp, duration, hashes));
	});

	return votes;
}
