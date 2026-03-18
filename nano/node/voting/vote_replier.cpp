#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/messages/confirm.hpp>
#include <nano/node/network.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/vote_spacing.hpp>
#include <nano/node/voting/vote_factory.hpp>
#include <nano/node/voting/vote_replier.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

nano::vote_replier::vote_replier (nano::vote_factory & factory_a, nano::ledger & ledger_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::network_params & network_params_a, nano::stats & stats_a, nano::logger & logger_a, std::shared_ptr<nano::transport::channel> inproc_channel_a) :
	factory (factory_a),
	ledger (ledger_a),
	network_params (network_params_a),
	stats (stats_a),
	logger (logger_a),
	spacing_impl{ std::make_unique<nano::vote_spacing> (network_params_a.voting.delay) },
	spacing{ *spacing_impl },
	broadcaster (factory_a, vote_processor_a, history_a, spacing, network_a, stats_a, std::move (inproc_channel_a))
{
}

nano::vote_replier::~vote_replier () = default;

std::size_t nano::vote_replier::reply (nano::secure::transaction const & transaction, request_type const & requests_a, std::shared_ptr<nano::transport::channel> const & channel_a)
{
	if (channel_a->max (nano::transport::traffic_type::vote_reply))
	{
		return 0;
	}

	// Collect candidates: look up blocks, check dependencies, classify via factory
	std::deque<nano::vote_factory::verified_candidate> final_candidates;

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

		// Classify first: only generate final vote replies
		if (!factory.is_final (transaction, block->qualified_root (), block->hash ()))
		{
			stats.inc (nano::stat::type::requests, nano::stat::detail::requests_non_final);
			continue;
		}

		// Check dependencies + get verified candidate
		auto candidate = factory.check_block (transaction, *block);
		if (!candidate)
		{
			stats.inc (nano::stat::type::requests, nano::stat::detail::requests_cannot_vote);
			continue;
		}

		final_candidates.push_back (std::move (*candidate));
		stats.inc (nano::stat::type::requests, nano::stat::detail::requests_final);
	}

	std::size_t replied = 0;

	// Generate and send final vote replies
	while (!final_candidates.empty ())
	{
		auto batch = broadcaster.collect_votable_batch (final_candidates, nano::stat::type::vote_replier);
		if (!batch.empty ())
		{
			stats.add (nano::stat::type::requests, nano::stat::detail::requests_generated_hashes, nano::stat::dir::in, batch.size ());

			broadcaster.vote (batch, [this, &channel_a] (std::shared_ptr<nano::vote> const & vote_a) {
				nano::messages::confirm_ack confirm{ network_params.network, vote_a };
				channel_a->send (confirm, nano::transport::traffic_type::vote_reply);
				stats.inc (nano::stat::type::requests, nano::stat::detail::requests_generated_votes, nano::stat::dir::in);
			});
			replied += batch.size ();
		}
	}

	return replied;
}

nano::container_info nano::vote_replier::container_info () const
{
	nano::container_info info;
	return info;
}
