#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/fwd.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace nano
{
class block;
class vote;

/**
 * Single point of responsibility for checking vote eligibility and signing votes.
 * All vote creation flows through this class.
 */
class vote_factory final
{
public:
	/**
	 * A candidate that has passed eligibility checks.
	 * Can only be created by vote_factory — enforces that all votes go through the check pipeline.
	 */
	class verified_candidate
	{
		friend class vote_factory;
		verified_candidate (nano::root const &, nano::block_hash const &, bool is_final);

	public:
		nano::root root;
		nano::block_hash hash;
		bool is_final;
	};

public:
	vote_factory (nano::ledger &, nano::wallets &);

	// --- Check individual candidates ---

	/** Normal vote check (read tx): block exists + dependencies_cemented. Returns verified_candidate with is_final=false. */
	std::optional<verified_candidate> check (nano::secure::read_transaction const &, nano::root const &, nano::block_hash const &) const;

	/** Final vote check (write tx): block exists + dependencies_cemented + final_vote.put(). Returns verified_candidate with is_final=true. */
	std::optional<verified_candidate> check_final (nano::secure::write_transaction const &, nano::root const &, nano::block_hash const &) const;

	/** Classification check (read tx): is this block eligible for a final vote reply? */
	bool is_final (nano::secure::transaction const &, nano::qualified_root const &, nano::block_hash const &) const;

	/** Reply-path check: verifies dependencies_cemented and classifies as final/non-final.
	 *  Block must already be looked up by the caller. Returns verified_candidate with appropriate is_final flag.
	 *  Returns nullopt if dependencies are not cemented. */
	std::optional<verified_candidate> check_block (nano::secure::transaction const &, nano::block const &) const;

	// --- Sign pre-verified candidates ---

	/** Creates one vote per local representative for the given verified candidates.
	 *  All candidates must have the same is_final value (votes are batched by type).
	 */
	std::vector<std::shared_ptr<nano::vote>> sign (std::vector<verified_candidate> const &) const;

private:
	nano::ledger & ledger;
	nano::wallets & wallets;
};
}
