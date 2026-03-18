#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/messages/confirm.hpp>
#include <nano/node/network.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/vote_spacing.hpp>
#include <nano/node/voting/vote_criteria.hpp>
#include <nano/node/voting/vote_replier.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>

nano::vote_replier::vote_replier (nano::ledger & ledger_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::network_params & network_params_a, nano::stats & stats_a, nano::logger & logger_a, std::shared_ptr<nano::transport::channel> inproc_channel_a) :
	ledger (ledger_a),
	network_params (network_params_a),
	stats (stats_a),
	logger (logger_a),
	spacing_impl{ std::make_unique<nano::vote_spacing> (network_params_a.voting.delay) },
	spacing{ *spacing_impl },
	broadcaster (wallets_a, vote_processor_a, history_a, spacing, network_a, stats_a, std::move (inproc_channel_a))
{
}

nano::vote_replier::~vote_replier () = default;

std::size_t nano::vote_replier::reply (nano::secure::transaction const & transaction, request_type const & requests_a, std::shared_ptr<nano::transport::channel> const & channel_a)
{
	if (channel_a->max (nano::transport::traffic_type::vote_reply))
	{
		return 0;
	}

	// Collect candidates: look up blocks, classify, check dependencies
	std::deque<nano::vote_broadcaster::candidate_t> final_candidates;

	for (auto const & [hash, root] : requests_a)
	{
		// Search for block in ledger
		std::shared_ptr<nano::block> block;

		// Ledger by hash
		block = ledger.any.block_get (transaction, hash);

		// Ledger by root
		if (!block && !root.is_zero ())
		{
			if (auto successor = ledger.any.block_successor (transaction, root.as_block_hash ()))
			{
				block = ledger.any.block_get (transaction, successor.value ());
			}

			if (!block)
			{
				if (auto info = ledger.any.account_get (transaction, root.as_account ()))
				{
					block = ledger.any.block_get (transaction, info->open_block);
				}
			}
		}

		if (!block)
		{
			stats.inc (nano::stat::type::requests, nano::stat::detail::requests_unknown);
			continue;
		}

		// Classify as final or non-final
		if (!nano::vote_criteria::is_final (transaction, ledger, block->qualified_root (), block->hash ()))
		{
			// Non-final vote replies are not generated
			stats.inc (nano::stat::type::requests, nano::stat::detail::requests_non_final);
			continue;
		}

		// Check dependencies are cemented before generating vote
		if (!ledger.dependencies_cemented (transaction, *block))
		{
			stats.inc (nano::stat::type::requests, nano::stat::detail::requests_cannot_vote);
			continue;
		}

		final_candidates.emplace_back (block->root (), block->hash ());
		stats.inc (nano::stat::type::requests, nano::stat::detail::requests_final);
	}

	std::size_t replied = 0;

	// Generate and send final vote replies
	while (!final_candidates.empty ())
	{
		auto [hashes, roots] = broadcaster.collect_votable_batch (final_candidates, nano::stat::type::vote_replier);
		if (!hashes.empty ())
		{
			stats.add (nano::stat::type::requests, nano::stat::detail::requests_generated_hashes, nano::stat::dir::in, hashes.size ());

			broadcaster.vote (hashes, roots, /* is_final */ true, [this, &channel_a] (std::shared_ptr<nano::vote> const & vote_a) {
				nano::messages::confirm_ack confirm{ network_params.network, vote_a };
				channel_a->send (confirm, nano::transport::traffic_type::vote_reply);
				stats.inc (nano::stat::type::requests, nano::stat::detail::requests_generated_votes, nano::stat::dir::in);
			});
			replied += hashes.size ();
		}
	}

	return replied;
}

nano::container_info nano::vote_replier::container_info () const
{
	nano::container_info info;
	return info;
}
