#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>

using namespace nano::rpc_api;

class epoch_upgrade_command final : public sync_command
{
	std::string_view name () const override
	{
		return "epoch_upgrade";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		auto epoch_str = params::required_string (ctx.params (), "epoch");
		if (!epoch_str)
			return command_result::error (epoch_str.error_message ());
		nano::epoch epoch = nano::epoch::invalid;
		if (*epoch_str == "1")
			epoch = nano::epoch::epoch_1;
		else if (*epoch_str == "2")
			epoch = nano::epoch::epoch_2;
		if (epoch == nano::epoch::invalid)
			return command_result::error ("Invalid epoch number");

		auto count_limit = params::optional_count (ctx.params ());
		auto threads_str = params::optional_string (ctx.params (), "threads");
		uint64_t threads = 0;
		if (!threads_str.empty ())
		{
			try
			{
				threads = std::stoull (threads_str);
			}
			catch (...)
			{
				return command_result::error ("Invalid threads count");
			}
		}

		auto key_text = params::required_string (ctx.params (), "key");
		if (!key_text)
			return command_result::error (key_text.error_message ());
		nano::raw_key prv;
		if (prv.decode_hex (*key_text))
			return command_result::error ("Bad private key");

		if (nano::pub_key (prv) != ctx.node.ledger.epoch_signer (ctx.node.ledger.epoch_link (epoch)))
			return command_result::error ("Epoch signer key mismatch");

		bool started = !ctx.node.epoch_upgrader.start (prv, epoch, count_limit, threads);
		return command_result::ok ({ { "started", started ? "1" : "0" } });
	}
};
REGISTER_RPC_COMMAND (epoch_upgrade_command);
