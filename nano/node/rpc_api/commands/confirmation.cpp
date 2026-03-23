#include <nano/lib/timer.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/election.hpp>
#include <nano/node/node.hpp>
#include <nano/node/online_reps.hpp>
#include <nano/node/repcrawler.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>

#include <boost/property_tree/json_parser.hpp>

using namespace nano::rpc_api;

class confirmation_active_command final : public sync_command
{
	std::string_view name () const override
	{
		return "confirmation_active";
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();
		uint64_t announcements = params::optional_uint64 (p, "announcements", 0);
		uint64_t confirmed = 0;

		boost::json::array elections;
		auto active_elections = ctx.node.active.list_active ();
		for (auto const & election : active_elections)
		{
			if (election->confirmation_request_count >= announcements)
			{
				if (!election->confirmed ())
				{
					elections.emplace_back (election->qualified_root.to_string ());
				}
				else
				{
					++confirmed;
				}
			}
		}

		boost::json::object result;
		result["confirmations"] = std::move (elections);
		result["unconfirmed"] = std::to_string (elections.size ());
		result["confirmed"] = std::to_string (confirmed);
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (confirmation_active_command);

class confirmation_history_command final : public sync_command
{
	std::string_view name () const override
	{
		return "confirmation_history";
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();

		nano::block_hash hash (0);
		auto hash_str = params::optional_string (p, "hash");
		if (!hash_str.empty ())
		{
			if (hash.decode_hex (hash_str))
				return command_result::error ("Bad hash");
		}

		boost::json::array elections;
		boost::json::object confirmation_stats;
		std::chrono::milliseconds running_total (0);

		for (auto const & status : ctx.node.active.recently_cemented.list (2000))
		{
			if (hash.is_zero () || status.winner->hash () == hash)
			{
				boost::json::object election;
				election["hash"] = status.winner->hash ().to_string ();
				election["duration"] = std::to_string (status.election_duration.count ());
				election["time"] = std::to_string (nano::milliseconds_since_epoch (status.election_end));
				election["tally"] = status.tally.to_string_dec ();
				election["final"] = status.final_tally.to_string_dec ();
				election["blocks"] = std::to_string (status.block_count);
				election["voters"] = std::to_string (status.voter_count);
				election["request_count"] = std::to_string (status.confirmation_request_count);
				elections.emplace_back (std::move (election));
			}
			running_total += status.election_duration;
		}

		confirmation_stats["count"] = std::to_string (elections.size ());
		if (elections.size () >= 1)
		{
			confirmation_stats["average"] = std::to_string (running_total.count () / elections.size ());
		}

		boost::json::object result;
		result["confirmation_stats"] = std::move (confirmation_stats);
		result["confirmations"] = std::move (elections);
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (confirmation_history_command);

class confirmation_info_command final : public sync_command
{
	std::string_view name () const override
	{
		return "confirmation_info";
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();

		bool const show_representatives = params::optional_bool (p, "representatives", false);
		bool const contents = params::optional_bool (p, "contents", true);
		bool const json_block_l = params::optional_bool (p, "json_block", false);

		auto root_str = params::optional_string (p, "root");
		if (root_str.empty ())
			return command_result::error ("Missing required parameter: root");

		nano::qualified_root root;
		if (root.decode_hex (root_str))
			return command_result::error ("Invalid root");

		auto election = ctx.node.active.election (root);
		if (election == nullptr || election->confirmed ())
			return command_result::error ("Active confirmation not found");

		auto info = election->current_status ();

		boost::json::object result;
		result["announcements"] = std::to_string (info.status.confirmation_request_count);
		result["voters"] = std::to_string (info.votes.size ());
		result["last_winner"] = info.status.winner->hash ().to_string ();

		nano::uint128_t total (0);
		boost::json::object blocks;
		for (auto const & [tally, block] : info.tally)
		{
			boost::json::object entry;
			entry["tally"] = tally.convert_to<std::string> ();
			total += tally;
			if (contents)
			{
				if (json_block_l)
				{
					boost::property_tree::ptree block_node_l;
					block->serialize_json (block_node_l);
					std::stringstream ss;
					boost::property_tree::write_json (ss, block_node_l);
					entry["contents"] = ss.str ();
				}
				else
				{
					std::string contents_str;
					block->serialize_json (contents_str);
					entry["contents"] = contents_str;
				}
			}
			if (show_representatives)
			{
				std::multimap<nano::uint128_t, nano::account, std::greater<nano::uint128_t>> reps;
				std::multimap<nano::uint128_t, nano::account, std::greater<nano::uint128_t>> reps_final;
				for (auto const & [representative, vote] : info.votes)
				{
					if (block->hash () == vote.hash)
					{
						auto amount = ctx.node.ledger.weight (representative);
						reps.emplace (amount, representative);
						if (vote.timestamp == std::numeric_limits<uint64_t>::max ())
						{
							reps_final.emplace (amount, representative);
						}
					}
				}

				boost::json::object reps_obj;
				boost::json::object reps_final_obj;
				for (auto const & [amount, representative] : reps)
				{
					reps_obj[representative.to_account ()] = amount.convert_to<std::string> ();
				}
				for (auto const & [amount, representative] : reps_final)
				{
					reps_final_obj[representative.to_account ()] = amount.convert_to<std::string> ();
				}
				entry["representatives"] = std::move (reps_obj);
				entry["representatives_final"] = std::move (reps_final_obj);
			}
			blocks[block->hash ().to_string ()] = std::move (entry);
		}
		result["total_tally"] = total.convert_to<std::string> ();
		result["final_tally"] = info.status.final_tally.to_string_dec ();
		result["blocks"] = std::move (blocks);
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (confirmation_info_command);

class confirmation_quorum_command final : public sync_command
{
	std::string_view name () const override
	{
		return "confirmation_quorum";
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();

		boost::json::object result;
		result["quorum_delta"] = ctx.node.online_reps.delta ().convert_to<std::string> ();
		result["online_weight_quorum_percent"] = std::to_string (nano::online_reps::online_weight_quorum);
		result["online_weight_minimum"] = ctx.node.config.online_weight_minimum.to_string_dec ();
		result["online_stake_total"] = ctx.node.online_reps.online ().convert_to<std::string> ();
		result["trended_stake_total"] = ctx.node.online_reps.trended ().convert_to<std::string> ();
		result["peers_stake_total"] = ctx.node.rep_crawler.total_weight ().convert_to<std::string> ();

		if (params::optional_bool (p, "peer_details", false))
		{
			boost::json::array peers;
			for (auto & peer : ctx.node.rep_crawler.representatives ())
			{
				boost::json::object peer_node;
				peer_node["account"] = peer.account.to_account ();
				peer_node["ip"] = peer.channel->to_string ();
				peer_node["weight"] = nano::amount{ ctx.node.ledger.weight (peer.account) }.to_string_dec ();
				peers.emplace_back (std::move (peer_node));
			}
			result["peers"] = std::move (peers);
		}
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (confirmation_quorum_command);
