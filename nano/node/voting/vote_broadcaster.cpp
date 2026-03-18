#include <nano/lib/stats.hpp>
#include <nano/lib/vote.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/network.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/node/vote_spacing.hpp>
#include <nano/node/voting/vote_broadcaster.hpp>
#include <nano/node/voting/vote_factory.hpp>

nano::vote_broadcaster::vote_broadcaster (nano::vote_factory & factory_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::vote_spacing & spacing_a, nano::network & network_a, nano::stats & stats_a, std::shared_ptr<nano::transport::channel> inproc_channel_a) :
	factory (factory_a),
	vote_processor (vote_processor_a),
	history (history_a),
	spacing (spacing_a),
	network (network_a),
	stats (stats_a),
	inproc_channel{ std::move (inproc_channel_a) }
{
}

void nano::vote_broadcaster::vote (std::vector<nano::vote_factory::verified_candidate> const & candidates, std::function<void (std::shared_ptr<nano::vote> const &)> const & action_a)
{
	auto votes = factory.sign (candidates);
	for (auto const & vote_l : votes)
	{
		for (auto const & candidate : candidates)
		{
			history.add (candidate.root, candidate.hash, vote_l);
			spacing.flag (candidate.root, candidate.hash);
		}
		action_a (vote_l);
	}
}

void nano::vote_broadcaster::broadcast (std::shared_ptr<nano::vote> const & vote_a, nano::stat::type stat_type) const
{
	vote_processor.vote (vote_a, inproc_channel);

	auto sent_pr = network.flood_vote_pr (vote_a);
	auto sent_non_pr = network.flood_vote_non_pr (vote_a, 2.0f);

	stats.add (stat_type, nano::stat::detail::sent_pr, sent_pr);
	stats.add (stat_type, nano::stat::detail::sent_non_pr, sent_non_pr);
}

auto nano::vote_broadcaster::collect_votable_batch (std::deque<nano::vote_factory::verified_candidate> & candidates, nano::stat::type stat_type) -> std::vector<nano::vote_factory::verified_candidate>
{
	std::vector<nano::vote_factory::verified_candidate> result;
	std::vector<nano::root> seen_roots;
	result.reserve (nano::network::confirm_ack_hashes_max);
	seen_roots.reserve (nano::network::confirm_ack_hashes_max);
	while (!candidates.empty () && result.size () < nano::network::confirm_ack_hashes_max)
	{
		auto candidate = std::move (candidates.front ());
		candidates.pop_front ();
		if (std::find (seen_roots.begin (), seen_roots.end (), candidate.root) == seen_roots.end ())
		{
			if (spacing.votable (candidate.root, candidate.hash))
			{
				seen_roots.push_back (candidate.root);
				result.push_back (std::move (candidate));
			}
			else
			{
				stats.inc (stat_type, nano::stat::detail::generator_spacing);
			}
		}
	}
	return result;
}
