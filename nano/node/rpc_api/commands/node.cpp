#include <nano/lib/config.hpp>
#include <nano/lib/constants.hpp>
#include <nano/lib/keypair.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/version.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/election.hpp>
#include <nano/node/election_behavior.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/common.hpp>
#include <nano/store/ledger_store.hpp>

#include <iomanip>
#include <sstream>

namespace nano::rpc_api
{
// version
class version_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "version";
	}

	command_result handle (request_context const & ctx) override
	{
		boost::json::object response;
		response["rpc_version"] = "1";
		response["store_version"] = std::to_string (ctx.node.store_version ());
		response["protocol_version"] = std::to_string (ctx.node.network_params.network.protocol_version);
		response["node_vendor"] = "Nano " + std::string (NANO_VERSION_STRING);
		response["store_vendor"] = ctx.node.store.vendor_get ();
		response["network"] = std::string (ctx.node.network_params.network.get_current_network_as_string ());
		response["network_identifier"] = ctx.node.network_params.ledger.genesis->hash ().to_string ();
		response["build_info"] = std::string (BUILD_INFO);
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (version_command);

// uptime
class uptime_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "uptime";
	}

	command_result handle (request_context const & ctx) override
	{
		auto seconds = std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now () - ctx.node.startup_time).count ();
		boost::json::object response;
		response["seconds"] = std::to_string (seconds);
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (uptime_command);

// key_create
class key_create_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "key_create";
	}

	command_result handle (request_context const & ctx) override
	{
		nano::keypair pair;
		boost::json::object response;
		response["private"] = pair.prv.to_string ();
		response["public"] = pair.pub.to_string ();
		response["account"] = pair.pub.to_account ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (key_create_command);

// key_expand
class key_expand_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "key_expand";
	}

	command_result handle (request_context const & ctx) override
	{
		auto key_str = params::required_string (ctx.params (), "key");
		if (!key_str)
			return command_result::error (key_str.error_message ());

		nano::raw_key prv;
		if (prv.decode_hex (*key_str))
			return command_result::error ("Bad private key");

		nano::public_key pub (nano::pub_key (prv));
		boost::json::object response;
		response["private"] = prv.to_string ();
		response["public"] = pub.to_string ();
		response["account"] = pub.to_account ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (key_expand_command);

// nano_to_raw
class nano_to_raw_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "nano_to_raw";
	}

	command_result handle (request_context const & ctx) override
	{
		auto amount = params::required_amount (ctx.params (), "amount");
		if (!amount)
			return command_result::error (amount.error_message ());

		auto result = amount->number () * nano::nano_ratio;
		if (result > amount->number ())
		{
			boost::json::object response;
			response["amount"] = result.convert_to<std::string> ();
			return command_result::ok (response);
		}
		else
		{
			return command_result::error ("Amount too big");
		}
	}
};
REGISTER_RPC_COMMAND (nano_to_raw_command);

// raw_to_nano
class raw_to_nano_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "raw_to_nano";
	}

	command_result handle (request_context const & ctx) override
	{
		auto amount = params::required_amount (ctx.params (), "amount");
		if (!amount)
			return command_result::error (amount.error_message ());

		auto result = amount->number () / nano::nano_ratio;
		boost::json::object response;
		response["amount"] = result.convert_to<std::string> ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (raw_to_nano_command);

// node_id
class node_id_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "node_id";
	}

	command_result handle (request_context const & ctx) override
	{
		boost::json::object response;
		response["public"] = ctx.node.node_id.pub.to_string ();
		response["as_account"] = ctx.node.node_id.pub.to_account ();
		response["node_id"] = ctx.node.node_id.pub.to_node_id ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (node_id_command);

// node_id_delete
class node_id_delete_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "node_id_delete";
	}

	command_result handle (request_context const & ctx) override
	{
		boost::json::object response;
		response["deprecated"] = "1";
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (node_id_delete_command);

// stop - overrides execute() directly from command (not sync_command)
class stop_command : public command
{
public:
	std::string_view name () const override
	{
		return "stop";
	}
	bool requires_control () const override
	{
		return true;
	}

	void execute (request_context & ctx) override
	{
		boost::json::object response;
		response["success"] = "";
		ctx.respond (std::move (response));
		try
		{
			ctx.stop_callback ();
		}
		catch (std::exception const & ex)
		{
			release_assert (false, "unexpected exception in stop callback", ex.what ());
		}
	}
};
REGISTER_RPC_COMMAND (stop_command);

// available_supply
class available_supply_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "available_supply";
	}

	command_result handle (request_context const & ctx) override
	{
		auto genesis_balance = ctx.node.balance (ctx.node.network_params.ledger.genesis->account ());
		auto landing_balance = ctx.node.balance (nano::account ("059F68AAB29DE0D3A27443625C7EA9CDDB6517A8B76FE37727EF6A4D76832AD5"));
		auto faucet_balance = ctx.node.balance (nano::account ("8E319CE6F3025E5B2DF66DA7AB1467FE48F1679C13DD43BFDB29FA2E9FC40D3B"));
		auto burned_balance = (ctx.node.balance_pending (nano::account{}, false)).second;
		auto available = ctx.node.network_params.ledger.genesis_amount - genesis_balance - landing_balance - faucet_balance - burned_balance;
		boost::json::object response;
		response["available"] = available.convert_to<std::string> ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (available_supply_command);

// active_difficulty
class active_difficulty_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "active_difficulty";
	}

	command_result handle (request_context const & ctx) override
	{
		auto include_trend = params::optional_bool (ctx.params (), "include_trend", false);
		auto const multiplier_active = 1.0;
		auto const default_difficulty = ctx.node.default_difficulty (nano::work_version::work_1);
		auto const default_receive_difficulty = ctx.node.default_receive_difficulty (nano::work_version::work_1);
		auto const receive_current_denormalized = ctx.node.network_params.work.denormalized_multiplier (multiplier_active, ctx.node.network_params.work.epoch_2_receive);

		boost::json::object response;
		response["deprecated"] = "1";
		response["network_minimum"] = nano::to_string_hex (default_difficulty);
		response["network_receive_minimum"] = nano::to_string_hex (default_receive_difficulty);
		response["network_current"] = nano::to_string_hex (nano::difficulty::from_multiplier (multiplier_active, default_difficulty));
		response["network_receive_current"] = nano::to_string_hex (nano::difficulty::from_multiplier (receive_current_denormalized, default_receive_difficulty));
		response["multiplier"] = "1";

		if (include_trend)
		{
			boost::json::array trend;
			trend.push_back ("1.000000000000000");
			response["difficulty_trend"] = std::move (trend);
		}

		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (active_difficulty_command);

// election_statistics
class election_statistics_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "election_statistics";
	}

	command_result handle (request_context const & ctx) override
	{
		auto active_elections = ctx.node.active.list_active ();
		unsigned manual_count = 0;
		unsigned priority_count = 0;
		unsigned hinted_count = 0;
		unsigned optimistic_count = 0;
		unsigned total_count = 0;
		std::chrono::steady_clock::duration total_age{};
		auto now = std::chrono::steady_clock::now ();
		std::chrono::steady_clock::time_point oldest_election_start = now;

		for (auto const & election : active_elections)
		{
			total_count++;
			auto election_start = election->get_election_start ();
			auto age = now - election_start;
			total_age += age;
			oldest_election_start = std::min (oldest_election_start, election->get_election_start ());

			switch (election->behavior ())
			{
				case nano::election_behavior::manual:
					manual_count++;
					break;
				case nano::election_behavior::priority:
					priority_count++;
					break;
				case nano::election_behavior::hinted:
					hinted_count++;
					break;
				case nano::election_behavior::optimistic:
					optimistic_count++;
					break;
			}
		}

		auto utilization_percentage = (static_cast<double> (total_count * 100) / ctx.node.config.active_elections.size);
		auto max_election_age = std::chrono::duration_cast<std::chrono::milliseconds> (now - oldest_election_start).count ();
		auto average_election_age = total_count ? std::chrono::duration_cast<std::chrono::milliseconds> (total_age).count () / total_count : 0;

		std::stringstream stream_utilization;
		stream_utilization << std::fixed << std::setprecision (2) << utilization_percentage;

		boost::json::object response;
		response["manual"] = std::to_string (manual_count);
		response["priority"] = std::to_string (priority_count);
		response["hinted"] = std::to_string (hinted_count);
		response["optimistic"] = std::to_string (optimistic_count);
		response["total"] = std::to_string (total_count);
		response["aec_utilization_percentage"] = stream_utilization.str ();
		response["max_election_age"] = std::to_string (max_election_age);
		response["average_election_age"] = std::to_string (average_election_age);

		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (election_statistics_command);

// deterministic_key
class deterministic_key_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "deterministic_key";
	}

	command_result handle (request_context const & ctx) override
	{
		auto seed_str = params::required_string (ctx.params (), "seed");
		if (!seed_str)
			return command_result::error (seed_str.error_message ());

		auto index_str = params::required_string (ctx.params (), "index");
		if (!index_str)
			return command_result::error (index_str.error_message ());

		nano::raw_key seed;
		if (seed.decode_hex (*seed_str))
			return command_result::error ("Bad seed");

		try
		{
			uint32_t index = std::stoul (*index_str);
			nano::raw_key prv = nano::deterministic_key (seed, index);
			nano::public_key pub (nano::pub_key (prv));
			boost::json::object response;
			response["private"] = prv.to_string ();
			response["public"] = pub.to_string ();
			response["account"] = pub.to_account ();
			return command_result::ok (response);
		}
		catch (std::logic_error const &)
		{
			return command_result::error ("Invalid index");
		}
	}
};
REGISTER_RPC_COMMAND (deterministic_key_command);
}
