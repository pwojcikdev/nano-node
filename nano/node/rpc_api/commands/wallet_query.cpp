#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/confirmation_height.hpp>
#include <nano/store/ledger/pending.hpp>

using namespace nano::rpc_api;

// account_list: List all accounts in a wallet
class account_list_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "account_list";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		boost::json::array accounts;
		for (auto const & account : (*wallet)->accounts ())
		{
			accounts.push_back (boost::json::value (account.to_account ()));
		}
		return command_result::ok ({ { "accounts", accounts } });
	}
};
REGISTER_RPC_COMMAND (account_list_cmd);

// wallet_contains: Check if a wallet contains an account
class wallet_contains_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_contains";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());

		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		return command_result::ok ({ { "exists", (*wallet)->exists (*account) ? "1" : "0" } });
	}
};
REGISTER_RPC_COMMAND (wallet_contains_cmd);

// wallet_key_valid: Check if a wallet is unlocked (key is valid)
class wallet_key_valid_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_key_valid";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		auto valid = !(*wallet)->is_locked ();
		return command_result::ok ({ { "valid", valid ? "1" : "0" } });
	}
};
REGISTER_RPC_COMMAND (wallet_key_valid_cmd);

// wallet_export: Export wallet as JSON string
class wallet_export_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_export";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		std::string json;
		(*wallet)->serialize_json (json);
		return command_result::ok ({ { "json", json } });
	}
};
REGISTER_RPC_COMMAND (wallet_export_cmd);

// wallet_representative: Get the default representative for a wallet
class wallet_representative_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_representative";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		return command_result::ok ({ { "representative", (*wallet)->get_representative ().to_account () } });
	}
};
REGISTER_RPC_COMMAND (wallet_representative_cmd);

// wallet_lock: Lock a wallet
class wallet_lock_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_lock";
	}
	bool requires_control () const override
	{
		return true;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		nano::raw_key empty;
		empty.clear ();
		(*wallet)->store.password.value_set (empty);

		ctx.node.logger.warn (nano::log::type::rpc, "Wallet locked");
		return command_result::ok ({ { "locked", "1" } });
	}
};
REGISTER_RPC_COMMAND (wallet_lock_cmd);

// wallet_info (alias: wallet_balance_total): Comprehensive wallet statistics
class wallet_info_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_info";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		nano::uint128_t balance (0);
		nano::uint128_t receivable (0);
		uint64_t count (0);
		uint64_t block_count (0);
		uint64_t cemented_block_count (0);
		uint64_t deterministic_count (0);
		uint64_t adhoc_count (0);
		auto block_transaction = ctx.node.ledger.tx_begin_read ();
		for (auto const & account : (*wallet)->accounts ())
		{
			auto account_info = ctx.node.ledger.any.account_get (block_transaction, account);
			if (account_info)
			{
				block_count += account_info->block_count;
				balance += account_info->balance.number ();
			}

			nano::confirmation_height_info confirmation_info{};
			if (!ctx.node.store.confirmation_height.get (block_transaction, account, confirmation_info))
			{
				cemented_block_count += confirmation_info.height;
			}

			receivable += ctx.node.ledger.account_receivable (block_transaction, account);

			nano::key_type key_type_l ((*wallet)->key_type (account));
			if (key_type_l == nano::key_type::deterministic)
			{
				deterministic_count++;
			}
			else if (key_type_l == nano::key_type::adhoc)
			{
				adhoc_count++;
			}

			++count;
		}

		uint32_t deterministic_index ((*wallet)->get_deterministic_index ());
		return command_result::ok ({
		{ "balance", balance.convert_to<std::string> () },
		{ "pending", receivable.convert_to<std::string> () },
		{ "receivable", receivable.convert_to<std::string> () },
		{ "accounts_count", std::to_string (count) },
		{ "accounts_block_count", std::to_string (block_count) },
		{ "accounts_cemented_block_count", std::to_string (cemented_block_count) },
		{ "deterministic_count", std::to_string (deterministic_count) },
		{ "adhoc_count", std::to_string (adhoc_count) },
		{ "deterministic_index", std::to_string (deterministic_index) },
		});
	}
};
REGISTER_RPC_COMMAND (wallet_info_cmd);

// wallet_balance_total: alias for wallet_info
static struct
{
	struct reg
	{
		reg ()
		{
			registry::instance ().register_alias ("wallet_balance_total", "wallet_info");
		}
	} r;
} _wallet_balance_total_alias;

// wallet_frontiers: Get frontier blocks for all wallet accounts
class wallet_frontiers_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_frontiers";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		boost::json::object frontiers;
		auto block_transaction = ctx.node.ledger.tx_begin_read ();
		for (auto const & account : (*wallet)->accounts ())
		{
			auto latest = ctx.node.ledger.any.account_head (block_transaction, account);
			if (!latest.is_zero ())
			{
				frontiers[account.to_account ()] = latest.to_string ();
			}
		}
		return command_result::ok ({ { "frontiers", frontiers } });
	}
};
REGISTER_RPC_COMMAND (wallet_frontiers_cmd);

// wallet_balances: Get balances for all wallet accounts
class wallet_balances_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_balances";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		auto threshold = params::optional_threshold (ctx.params ());

		boost::json::object balances;
		auto block_transaction = ctx.node.ledger.tx_begin_read ();
		for (auto const & account : (*wallet)->accounts ())
		{
			nano::uint128_t balance_val = ctx.node.ledger.any.account_balance (block_transaction, account).value_or (0).number ();
			if (balance_val >= threshold.number ())
			{
				nano::uint128_t receivable_val = ctx.node.ledger.account_receivable (block_transaction, account);
				boost::json::object entry;
				entry["balance"] = balance_val.convert_to<std::string> ();
				entry["pending"] = receivable_val.convert_to<std::string> ();
				entry["receivable"] = receivable_val.convert_to<std::string> ();
				balances[account.to_account ()] = entry;
			}
		}
		return command_result::ok ({ { "balances", balances } });
	}
};
REGISTER_RPC_COMMAND (wallet_balances_cmd);
