#include <nano/node/active_elections.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/pending.hpp>

using namespace nano::rpc_api;

namespace
{
bool block_confirmed_local (nano::node & node, nano::secure::transaction & transaction, nano::block_hash const & hash, bool include_active, bool include_only_confirmed)
{
	bool is_confirmed = false;
	if (include_active && !include_only_confirmed)
	{
		is_confirmed = true;
	}
	else if (node.ledger.cemented.block_exists_or_pruned (transaction, hash))
	{
		is_confirmed = true;
	}
	else if (!include_only_confirmed)
	{
		auto block = node.ledger.any.block_get (transaction, hash);
		is_confirmed = (block != nullptr && !node.active.active (*block));
	}
	return is_confirmed;
}
}

// wallet_receivable: Get receivable blocks for wallet accounts
class wallet_receivable_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_receivable";
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

		auto count = params::optional_count (ctx.params ());
		auto threshold = params::optional_threshold (ctx.params ());
		bool const source = params::optional_bool (ctx.params (), "source", false);
		bool const min_version = params::optional_bool (ctx.params (), "min_version", false);
		bool const include_active = params::optional_bool (ctx.params (), "include_active", false);
		bool const include_only_confirmed = params::optional_bool (ctx.params (), "include_only_confirmed", true);

		boost::json::object pending;
		auto block_transaction = ctx.node.ledger.tx_begin_read ();
		for (auto const & account : (*wallet)->accounts ())
		{
			boost::json::object peers_l;
			boost::json::array peers_simple;
			uint64_t entry_count = 0;
			for (auto ii (ctx.node.store.pending.begin (block_transaction, nano::pending_key (account, 0))), nn (ctx.node.store.pending.end (block_transaction)); ii != nn && nano::pending_key (ii->first).account == account && entry_count < count; ++ii)
			{
				nano::pending_key key (ii->first);
				if (block_confirmed_local (ctx.node, block_transaction, key.hash, include_active, include_only_confirmed))
				{
					if (threshold.is_zero () && !source)
					{
						peers_simple.push_back (boost::json::value (key.hash.to_string ()));
						entry_count++;
					}
					else
					{
						nano::pending_info info (ii->second);
						if (info.amount.number () >= threshold.number ())
						{
							if (source || min_version)
							{
								boost::json::object pending_tree;
								pending_tree["amount"] = info.amount.number ().convert_to<std::string> ();
								if (source)
								{
									pending_tree["source"] = info.source.to_account ();
								}
								peers_l[key.hash.to_string ()] = pending_tree;
							}
							else
							{
								peers_l[key.hash.to_string ()] = info.amount.number ().convert_to<std::string> ();
							}
							entry_count++;
						}
					}
				}
			}
			if (!peers_simple.empty ())
			{
				pending[account.to_account ()] = peers_simple;
			}
			else if (!peers_l.empty ())
			{
				pending[account.to_account ()] = peers_l;
			}
		}
		return command_result::ok ({ { "blocks", pending } });
	}
};
REGISTER_RPC_COMMAND (wallet_receivable_cmd);

// wallet_pending: Deprecated alias for wallet_receivable
class wallet_pending_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_pending";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		// Delegate to wallet_receivable logic
		auto * cmd = registry::instance ().find ("wallet_receivable");
		if (cmd)
		{
			// Use execute directly, but for sync we recreate
			// Instead, just add deprecated and inline
		}
		// Inline the logic with deprecated flag
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		auto count = params::optional_count (ctx.params ());
		auto threshold = params::optional_threshold (ctx.params ());
		bool const source = params::optional_bool (ctx.params (), "source", false);
		bool const include_active = params::optional_bool (ctx.params (), "include_active", false);
		bool const include_only_confirmed = params::optional_bool (ctx.params (), "include_only_confirmed", true);

		boost::json::object pending;
		auto block_transaction = ctx.node.ledger.tx_begin_read ();
		for (auto const & account : (*wallet)->accounts ())
		{
			boost::json::object peers_l;
			boost::json::array peers_simple;
			uint64_t entry_count = 0;
			for (auto ii (ctx.node.store.pending.begin (block_transaction, nano::pending_key (account, 0))), nn (ctx.node.store.pending.end (block_transaction)); ii != nn && nano::pending_key (ii->first).account == account && entry_count < count; ++ii)
			{
				nano::pending_key key (ii->first);
				if (block_confirmed_local (ctx.node, block_transaction, key.hash, include_active, include_only_confirmed))
				{
					if (threshold.is_zero () && !source)
					{
						peers_simple.push_back (boost::json::value (key.hash.to_string ()));
						entry_count++;
					}
					else
					{
						nano::pending_info info (ii->second);
						if (info.amount.number () >= threshold.number ())
						{
							if (source)
							{
								boost::json::object pending_tree;
								pending_tree["amount"] = info.amount.number ().convert_to<std::string> ();
								pending_tree["source"] = info.source.to_account ();
								peers_l[key.hash.to_string ()] = pending_tree;
							}
							else
							{
								peers_l[key.hash.to_string ()] = info.amount.number ().convert_to<std::string> ();
							}
							entry_count++;
						}
					}
				}
			}
			if (!peers_simple.empty ())
			{
				pending[account.to_account ()] = peers_simple;
			}
			else if (!peers_l.empty ())
			{
				pending[account.to_account ()] = peers_l;
			}
		}
		boost::json::object result;
		result["deprecated"] = "1";
		result["blocks"] = pending;
		return command_result::ok (result);
	}
};
REGISTER_RPC_COMMAND (wallet_pending_cmd);

// wallet_work_get: Get cached work for all wallet accounts
class wallet_work_get_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_work_get";
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

		boost::json::object works;
		for (auto const & account : (*wallet)->accounts ())
		{
			uint64_t work (0);
			auto error_work = (*wallet)->get_work (account, work);
			(void)error_work;
			works[account.to_account ()] = nano::to_string_hex (work);
		}
		return command_result::ok ({ { "works", works } });
	}
};
REGISTER_RPC_COMMAND (wallet_work_get_cmd);

// wallet_history: Transaction history for wallet accounts (STUB - complex due to history_visitor)
// This is left as a stub because it depends on the legacy history_visitor pattern
// which is tightly coupled to the property tree-based json_handler.
// TODO: Migrate when history_visitor is decoupled
// REGISTER_RPC_COMMAND (wallet_history_cmd);

// wallet_ledger: Ledger info for wallet accounts
class wallet_ledger_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_ledger";
	}
	bool requires_control () const override
	{
		return true;
	}

	command_result handle (request_context const & ctx) override
	{
		bool const representative = params::optional_bool (ctx.params (), "representative", false);
		bool const weight = params::optional_bool (ctx.params (), "weight", false);
		bool const pending_flag = params::optional_bool (ctx.params (), "pending", false);
		bool const receivable = params::optional_bool (ctx.params (), "receivable", pending_flag);
		uint64_t modified_since = params::optional_uint64 (ctx.params (), "modified_since", 0);

		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		boost::json::object accounts;
		auto block_transaction = ctx.node.ledger.tx_begin_read ();
		for (auto const & account : (*wallet)->accounts ())
		{
			auto info = ctx.node.ledger.any.account_get (block_transaction, account);
			if (info)
			{
				if (info->modified >= modified_since)
				{
					boost::json::object entry;
					entry["frontier"] = info->head.to_string ();
					entry["open_block"] = info->open_block.to_string ();
					entry["representative_block"] = ctx.node.ledger.representative_block (block_transaction, info->head).to_string ();
					std::string balance = nano::uint128_union (info->balance).to_string_dec ();
					entry["balance"] = balance;
					entry["modified_timestamp"] = std::to_string (info->modified);
					entry["block_count"] = std::to_string (info->block_count);
					if (representative)
					{
						entry["representative"] = info->representative.to_account ();
					}
					if (weight)
					{
						auto account_weight = ctx.node.ledger.weight_exact (block_transaction, account);
						entry["weight"] = account_weight.convert_to<std::string> ();
					}
					if (receivable)
					{
						auto account_receivable = ctx.node.ledger.account_receivable (block_transaction, account);
						entry["pending"] = account_receivable.convert_to<std::string> ();
						entry["receivable"] = account_receivable.convert_to<std::string> ();
					}
					accounts[account.to_account ()] = entry;
				}
			}
		}
		return command_result::ok ({ { "accounts", accounts } });
	}
};
REGISTER_RPC_COMMAND (wallet_ledger_cmd);

// wallet_republish: Republish blocks for wallet accounts
class wallet_republish_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_republish";
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

		auto count = params::required_count (ctx.params ());
		if (!count)
			return command_result::error (count.error_message ());

		boost::json::array blocks;
		std::deque<std::shared_ptr<nano::block>> republish_bundle;
		auto block_transaction = ctx.node.ledger.tx_begin_read ();
		for (auto const & account : (*wallet)->accounts ())
		{
			auto latest = ctx.node.ledger.any.account_head (block_transaction, account);
			std::shared_ptr<nano::block> block;
			std::vector<nano::block_hash> hashes;
			while (!latest.is_zero () && hashes.size () < *count)
			{
				hashes.push_back (latest);
				block = ctx.node.ledger.any.block_get (block_transaction, latest);
				if (block != nullptr)
				{
					latest = block->previous ();
				}
				else
				{
					latest.clear ();
				}
			}
			std::reverse (hashes.begin (), hashes.end ());
			for (auto & hash : hashes)
			{
				block = ctx.node.ledger.any.block_get (block_transaction, hash);
				republish_bundle.push_back (std::move (block));
				blocks.push_back (boost::json::value (hash.to_string ()));
			}
		}
		ctx.node.network.flood_block_many (std::move (republish_bundle), nano::transport::traffic_type::keepalive, 25ms);
		return command_result::ok ({
		{ "success", "" },
		{ "blocks", blocks },
		});
	}
};
REGISTER_RPC_COMMAND (wallet_republish_cmd);

// wallet_seed: Return wallet seed (requires unlock)
class wallet_seed_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_seed";
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

		nano::raw_key seed;
		if (!(*wallet)->get_seed (seed))
		{
			return command_result::ok ({ { "seed", seed.to_string () } });
		}
		else
		{
			return command_result::error ("Wallet locked");
		}
	}
};
REGISTER_RPC_COMMAND (wallet_seed_cmd);
