#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/fwd.hpp>

namespace nano
{
class ledger;
}

namespace nano::vote_criteria
{
/**
 * Normal vote check (read transaction): block exists + dependencies_cemented
 * Used by: normal vote_generator's process_batch
 */
bool should_vote (nano::secure::read_transaction const &, nano::ledger &, nano::root const &, nano::block_hash const &);

/**
 * Final vote check - write (write transaction): block exists + dependencies_cemented + final_vote.put()
 * Used by: final_vote_generator's process_batch (inserts into final_vote store)
 */
bool should_vote_final (nano::secure::write_transaction const &, nano::ledger &, nano::root const &, nano::block_hash const &);

/**
 * Final vote check - read (read transaction): final_vote.get() matches hash OR block is cemented
 * Used by: vote_replier to classify whether reply should be a final vote
 * Does NOT write to the final_vote store
 */
bool is_final (nano::secure::transaction const &, nano::ledger &, nano::qualified_root const &, nano::block_hash const &);
}
