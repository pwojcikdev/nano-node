#include <nano/node/active_elections.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
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

// accounts_receivable: Get receivable blocks for multiple accounts
class accounts_receivable_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "accounts_receivable";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto count = params::optional_count (ctx.params ());
		auto threshold = params::optional_threshold (ctx.params ());
		bool const source = params::optional_bool (ctx.params (), "source", false);
		bool const include_active = params::optional_bool (ctx.params (), "include_active", false);
		bool const include_only_confirmed = params::optional_bool (ctx.params (), "include_only_confirmed", true);
		bool const sorting = params::optional_bool (ctx.params (), "sorting", false);
		auto simple = threshold.is_zero () && !source && !sorting;

		auto accounts_arr = params::required_string_array (ctx.params (), "accounts");
		if (!accounts_arr)
			return command_result::error (accounts_arr.error_message ());

		boost::json::object pending;
		auto transaction = ctx.node.ledger.tx_begin_read ();

		for (auto const & acc_str : *accounts_arr)
		{
			nano::account account;
			if (account.decode_account (acc_str))
				return command_result::error ("Bad account number");

			if (simple)
			{
				boost::json::array peers_simple;
				for (auto i (ctx.node.store.pending.begin (transaction, nano::pending_key (account, 0))), n (ctx.node.store.pending.end (transaction)); i != n && nano::pending_key (i->first).account == account && peers_simple.size () < count; ++i)
				{
					nano::pending_key const & key (i->first);
					if (block_confirmed_local (ctx.node, transaction, key.hash, include_active, include_only_confirmed))
					{
						peers_simple.push_back (boost::json::value (key.hash.to_string ()));
					}
				}
				if (!peers_simple.empty ())
				{
					pending[account.to_account ()] = peers_simple;
				}
			}
			else
			{
				// Collect entries for possible sorting
				struct pending_entry
				{
					std::string hash;
					nano::uint128_t amount;
					std::string source_account;
				};
				std::vector<pending_entry> entries;

				for (auto i (ctx.node.store.pending.begin (transaction, nano::pending_key (account, 0))), n (ctx.node.store.pending.end (transaction)); i != n && nano::pending_key (i->first).account == account && entries.size () < count; ++i)
				{
					nano::pending_key const & key (i->first);
					if (block_confirmed_local (ctx.node, transaction, key.hash, include_active, include_only_confirmed))
					{
						nano::pending_info const & info (i->second);
						if (info.amount.number () >= threshold.number ())
						{
							pending_entry pe;
							pe.hash = key.hash.to_string ();
							pe.amount = info.amount.number ();
							if (source)
							{
								pe.source_account = info.source.to_account ();
							}
							entries.push_back (pe);
						}
					}
				}

				if (sorting)
				{
					std::sort (entries.begin (), entries.end (), [] (auto const & a, auto const & b) {
						return a.amount > b.amount;
					});
				}

				boost::json::object peers_l;
				for (auto const & entry : entries)
				{
					if (source)
					{
						boost::json::object pending_tree;
						pending_tree["amount"] = entry.amount.convert_to<std::string> ();
						pending_tree["source"] = entry.source_account;
						peers_l[entry.hash] = pending_tree;
					}
					else
					{
						peers_l[entry.hash] = entry.amount.convert_to<std::string> ();
					}
				}
				if (!peers_l.empty ())
				{
					pending[account.to_account ()] = peers_l;
				}
			}
		}
		return command_result::ok ({ { "blocks", pending } });
	}
};
REGISTER_RPC_COMMAND (accounts_receivable_cmd);

// accounts_pending: Deprecated alias for accounts_receivable
class accounts_pending_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "accounts_pending";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		// Re-invoke the same logic with deprecated flag
		auto count = params::optional_count (ctx.params ());
		auto threshold = params::optional_threshold (ctx.params ());
		bool const source = params::optional_bool (ctx.params (), "source", false);
		bool const include_active = params::optional_bool (ctx.params (), "include_active", false);
		bool const include_only_confirmed = params::optional_bool (ctx.params (), "include_only_confirmed", true);
		bool const sorting = params::optional_bool (ctx.params (), "sorting", false);
		auto simple = threshold.is_zero () && !source && !sorting;

		auto accounts_arr = params::required_string_array (ctx.params (), "accounts");
		if (!accounts_arr)
			return command_result::error (accounts_arr.error_message ());

		boost::json::object pending;
		auto transaction = ctx.node.ledger.tx_begin_read ();

		for (auto const & acc_str : *accounts_arr)
		{
			nano::account account;
			if (account.decode_account (acc_str))
				return command_result::error ("Bad account number");

			if (simple)
			{
				boost::json::array peers_simple;
				for (auto i (ctx.node.store.pending.begin (transaction, nano::pending_key (account, 0))), n (ctx.node.store.pending.end (transaction)); i != n && nano::pending_key (i->first).account == account && peers_simple.size () < count; ++i)
				{
					nano::pending_key const & key (i->first);
					if (block_confirmed_local (ctx.node, transaction, key.hash, include_active, include_only_confirmed))
					{
						peers_simple.push_back (boost::json::value (key.hash.to_string ()));
					}
				}
				if (!peers_simple.empty ())
				{
					pending[account.to_account ()] = peers_simple;
				}
			}
			else
			{
				struct pending_entry
				{
					std::string hash;
					nano::uint128_t amount;
					std::string source_account;
				};
				std::vector<pending_entry> entries;

				for (auto i (ctx.node.store.pending.begin (transaction, nano::pending_key (account, 0))), n (ctx.node.store.pending.end (transaction)); i != n && nano::pending_key (i->first).account == account && entries.size () < count; ++i)
				{
					nano::pending_key const & key (i->first);
					if (block_confirmed_local (ctx.node, transaction, key.hash, include_active, include_only_confirmed))
					{
						nano::pending_info const & info (i->second);
						if (info.amount.number () >= threshold.number ())
						{
							pending_entry pe;
							pe.hash = key.hash.to_string ();
							pe.amount = info.amount.number ();
							if (source)
							{
								pe.source_account = info.source.to_account ();
							}
							entries.push_back (pe);
						}
					}
				}

				if (sorting)
				{
					std::sort (entries.begin (), entries.end (), [] (auto const & a, auto const & b) {
						return a.amount > b.amount;
					});
				}

				boost::json::object peers_l;
				for (auto const & entry : entries)
				{
					if (source)
					{
						boost::json::object pending_tree;
						pending_tree["amount"] = entry.amount.convert_to<std::string> ();
						pending_tree["source"] = entry.source_account;
						peers_l[entry.hash] = pending_tree;
					}
					else
					{
						peers_l[entry.hash] = entry.amount.convert_to<std::string> ();
					}
				}
				if (!peers_l.empty ())
				{
					pending[account.to_account ()] = peers_l;
				}
			}
		}
		boost::json::object result;
		result["deprecated"] = "1";
		result["blocks"] = pending;
		return command_result::ok (result);
	}
};
REGISTER_RPC_COMMAND (accounts_pending_cmd);
