#include <nano/lib/stats.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/vote.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/network.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/node/vote_spacing.hpp>
#include <nano/node/voting/vote_broadcaster.hpp>
#include <nano/node/wallet.hpp>

nano::vote_broadcaster::vote_broadcaster (nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::vote_spacing & spacing_a, nano::network & network_a, nano::stats & stats_a, std::shared_ptr<nano::transport::channel> inproc_channel_a) :
	wallets (wallets_a),
	vote_processor (vote_processor_a),
	history (history_a),
	spacing (spacing_a),
	network (network_a),
	stats (stats_a),
	inproc_channel{ std::move (inproc_channel_a) }
{
}

void nano::vote_broadcaster::vote (std::vector<nano::block_hash> const & hashes_a, std::vector<nano::root> const & roots_a, bool is_final, std::function<void (std::shared_ptr<nano::vote> const &)> const & action_a)
{
	debug_assert (hashes_a.size () == roots_a.size ());
	std::vector<std::shared_ptr<nano::vote>> votes_l;
	wallets.foreach_representative ([&hashes_a, &votes_l, is_final] (nano::public_key const & pub_a, nano::raw_key const & prv_a) {
		auto timestamp = is_final ? nano::vote::timestamp_max : nano::milliseconds_since_epoch ();
		uint8_t duration = is_final ? nano::vote::duration_max : /*8192ms*/ 0x9;
		votes_l.emplace_back (std::make_shared<nano::vote> (pub_a, prv_a, timestamp, duration, hashes_a));
	});
	for (auto const & vote_l : votes_l)
	{
		for (std::size_t i (0), n (hashes_a.size ()); i != n; ++i)
		{
			history.add (roots_a[i], hashes_a[i], vote_l);
			spacing.flag (roots_a[i], hashes_a[i]);
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

auto nano::vote_broadcaster::collect_votable_batch (std::deque<candidate_t> & candidates, nano::stat::type stat_type) -> batch_result
{
	batch_result result;
	result.hashes.reserve (nano::network::confirm_ack_hashes_max);
	result.roots.reserve (nano::network::confirm_ack_hashes_max);
	while (!candidates.empty () && result.hashes.size () < nano::network::confirm_ack_hashes_max)
	{
		auto const & [root, hash] = candidates.front ();
		if (std::find (result.roots.begin (), result.roots.end (), root) == result.roots.end ())
		{
			if (spacing.votable (root, hash))
			{
				result.roots.push_back (root);
				result.hashes.push_back (hash);
			}
			else
			{
				stats.inc (stat_type, nano::stat::detail::generator_spacing);
			}
		}
		candidates.pop_front ();
	}
	return result;
}
