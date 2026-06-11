#include <nano/node/bootstrap/frontier_scan.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/pending.hpp>

#include <algorithm>

namespace nano::bootstrap
{
frontier_round::frontier_round (nano::frontier_scan_config const & config_a, nano::account position_a, nano::account range_end_a) :
	config{ config_a },
	position{ position_a },
	range_end{ range_end_a }
{
}

void frontier_round::feed (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	// Accounts not preceding the queried start is enforced by response verification
	debug_assert (std::all_of (frontiers.begin (), frontiers.end (), [&] (auto const & pair) { return pair.first.number () >= position.number (); }));

	completed_m += 1;

	for (auto const & [account, _] : frontiers)
	{
		// Only consider candidates that actually advance the scan position
		if (account.number () > position.number ())
		{
			candidates.insert (account);
		}
	}

	// Bound the advancement by keeping only the smallest candidates, so no frontiers are skipped
	while (candidates.size () > config.candidates)
	{
		candidates.erase (std::prev (candidates.end ()));
	}
}

bool frontier_round::done () const
{
	return completed_m >= config.consideration_count && !candidates.empty ();
}

bool frontier_round::empty_range () const
{
	return completed_m >= config.consideration_count * 2 && candidates.empty ();
}

bool frontier_round::settled () const
{
	return done () || empty_range ();
}

std::optional<nano::account> frontier_round::conclude () const
{
	if (!candidates.empty ())
	{
		return *candidates.rbegin (); // Advance to the largest kept candidate
	}
	if (completed_m > 0)
	{
		return range_end; // Unanimous "nothing past this position", the scan should wrap the range
	}
	return std::nullopt; // Nothing learned, the position should be retried
}

size_t frontier_round::completed () const
{
	return completed_m;
}

size_t frontier_round::candidate_count () const
{
	return candidates.size ();
}

frontier_classification classify_frontiers (nano::ledger & ledger, nano::secure::transaction const & transaction, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	release_assert (!frontiers.empty ());

	// Accounts must be passed in ascending order
	debug_assert (std::adjacent_find (frontiers.begin (), frontiers.end (), [] (auto const & lhs, auto const & rhs) {
		return lhs.first.number () >= rhs.first.number ();
	})
	== frontiers.end ());

	frontier_classification result;

	auto const start = frontiers.front ().first;
	auto account_crawler = ledger.store.account.crawl (transaction, start);
	auto pending_crawler = ledger.store.pending.crawl (transaction, start);

	auto block_exists = [&] (nano::block_hash const & hash) {
		return ledger.any.block_exists_or_pruned (transaction, hash);
	};

	auto should_prioritize = [&] (nano::account const & account, nano::block_hash const & frontier) {
		account_crawler.skip_to (account);
		pending_crawler.skip_to (account);

		// Check if account exists in our ledger
		if (account_crawler && account_crawler->first == account)
		{
			// Check for frontier mismatch
			if (account_crawler->second.head != frontier)
			{
				// Check if frontier block exists in our ledger
				if (!block_exists (frontier))
				{
					result.outdated++;
					return true; // Frontier is outdated
				}
			}
			return false; // Account exists and frontier is up-to-date
		}

		// Check if account has pending blocks in our ledger
		if (pending_crawler && pending_crawler->first.account == account)
		{
			result.pending++;
			return true; // Account doesn't exist but has pending blocks in the ledger
		}

		return false; // Account doesn't exist in the ledger and has no pending blocks, can't be prioritized right now
	};

	for (auto const & [account, frontier] : frontiers)
	{
		if (should_prioritize (account, frontier))
		{
			result.prioritize.push_back (account);
		}
	}

	return result;
}
}
