#include <nano/lib/block_sideband.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/work.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>

using namespace nano::rpc_api;

class work_cancel_command final : public sync_command
{
	std::string_view name () const override
	{
		return "work_cancel";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		auto hash = params::required_hash (ctx.params ());
		if (!hash)
			return command_result::error (hash.error_message ());
		ctx.node.observers.work_cancel.notify (*hash);
		return command_result::ok ({ { "success", "" } });
	}
};
REGISTER_RPC_COMMAND (work_cancel_command);

class work_peer_add_command final : public sync_command
{
	std::string_view name () const override
	{
		return "work_peer_add";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		auto address = params::required_string (ctx.params (), "address");
		if (!address)
			return command_result::error (address.error_message ());
		auto port_str = params::required_string (ctx.params (), "port");
		if (!port_str)
			return command_result::error (port_str.error_message ());
		uint16_t port;
		if (nano::parse_port (*port_str, port))
			return command_result::error ("Invalid port");
		ctx.node.config.work_peers.push_back (std::make_pair (*address, port));
		return command_result::ok ({ { "success", "" } });
	}
};
REGISTER_RPC_COMMAND (work_peer_add_command);

class work_peers_command final : public sync_command
{
	std::string_view name () const override
	{
		return "work_peers";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		boost::json::array work_peers;
		for (auto const & peer : ctx.node.config.work_peers)
			work_peers.push_back (boost::json::value (peer.first + ":" + std::to_string (peer.second)));
		return command_result::ok ({ { "work_peers", work_peers } });
	}
};
REGISTER_RPC_COMMAND (work_peers_command);

class work_peers_clear_command final : public sync_command
{
	std::string_view name () const override
	{
		return "work_peers_clear";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		ctx.node.config.work_peers.clear ();
		return command_result::ok ({ { "success", "" } });
	}
};
REGISTER_RPC_COMMAND (work_peers_clear_command);

class work_validate_command final : public sync_command
{
	std::string_view name () const override
	{
		return "work_validate";
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();

		auto hash_r = params::required_hash (p);
		if (!hash_r)
			return command_result::error (hash_r.error_message ());
		auto hash = *hash_r;

		// Parse work (hex)
		auto work_str = params::optional_string (p, "work");
		if (work_str.empty ())
			return command_result::error ("Missing required parameter: work");
		uint64_t work = 0;
		if (nano::from_string_hex (work_str, work))
			return command_result::error ("Bad work");

		// Parse work version
		nano::work_version work_version = nano::work_version::work_1;
		auto version_str = params::optional_string (p, "version");
		if (!version_str.empty ())
		{
			if (version_str == nano::to_string (nano::work_version::work_1))
			{
				work_version = nano::work_version::work_1;
			}
			else
			{
				return command_result::error ("Bad work version");
			}
		}

		// Parse difficulty (hex, optional)
		uint64_t difficulty = ctx.node.default_difficulty (work_version);
		bool difficulty_provided = false;
		auto difficulty_str = params::optional_string (p, "difficulty");
		if (!difficulty_str.empty ())
		{
			if (nano::from_string_hex (difficulty_str, difficulty))
				return command_result::error ("Bad difficulty");
			difficulty_provided = true;
		}

		// Parse multiplier (optional, overrides difficulty)
		auto multiplier_str = params::optional_string (p, "multiplier");
		if (!multiplier_str.empty ())
		{
			try
			{
				double multiplier = std::stod (multiplier_str);
				if (multiplier > 0.)
				{
					difficulty = nano::difficulty::from_multiplier (multiplier, ctx.node.default_difficulty (work_version));
				}
				else
				{
					return command_result::error ("Bad multiplier");
				}
			}
			catch (...)
			{
				return command_result::error ("Bad multiplier");
			}
		}

		auto result_difficulty = ctx.node.network_params.work.difficulty (work_version, hash, work);
		boost::json::object result;
		if (difficulty_provided || !multiplier_str.empty ())
		{
			result["valid"] = (result_difficulty >= difficulty) ? "1" : "0";
		}
		result["valid_all"] = (result_difficulty >= ctx.node.default_difficulty (work_version)) ? "1" : "0";
		result["valid_receive"] = (result_difficulty >= ctx.node.network_params.work.threshold (work_version, nano::block_details (nano::epoch::epoch_2, false, true, false))) ? "1" : "0";
		result["difficulty"] = nano::to_string_hex (result_difficulty);
		auto result_multiplier = nano::difficulty::to_multiplier (result_difficulty, ctx.node.default_difficulty (work_version));
		result["multiplier"] = nano::to_string (result_multiplier);
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (work_validate_command);
