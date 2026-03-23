#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger/pruned.hpp>
#include <nano/store/ledger_store.hpp>

using namespace nano::rpc_api;

class frontiers_command final : public sync_command
{
	std::string_view name () const override
	{
		return "frontiers";
	}
	command_result handle (request_context const & ctx) override
	{
		auto start = params::required_account (ctx.params ());
		if (!start)
			return command_result::error (start.error_message ());
		auto count = params::required_count (ctx.params ());
		if (!count)
			return command_result::error (count.error_message ());
		boost::json::object frontiers;
		auto transaction = ctx.node.ledger.tx_begin_read ();
		for (auto i = ctx.node.store.account.begin (transaction, *start),
				  n = ctx.node.store.account.end (transaction);
			 i != n && frontiers.size () < *count; ++i)
		{
			frontiers[i->first.to_account ()] = i->second.head.to_string ();
		}
		return command_result::ok ({ { "frontiers", frontiers } });
	}
};
REGISTER_RPC_COMMAND (frontiers_command);

class pruned_exists_command final : public sync_command
{
	std::string_view name () const override
	{
		return "pruned_exists";
	}
	command_result handle (request_context const & ctx) override
	{
		auto hash = params::required_hash (ctx.params ());
		if (!hash)
			return command_result::error (hash.error_message ());
		if (!ctx.node.ledger.pruning)
			return command_result::error ("Pruning is not enabled");
		auto transaction = ctx.node.store.tx_begin_read ();
		auto exists = ctx.node.store.pruned.exists (transaction, *hash);
		return command_result::ok ({ { "exists", exists ? "1" : "0" } });
	}
};
REGISTER_RPC_COMMAND (pruned_exists_command);

class ledger_command final : public sync_command
{
	std::string_view name () const override
	{
		return "ledger";
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();

		auto count = params::optional_count (p);
		auto threshold = params::optional_threshold (p);

		nano::account start{};
		auto account_text = params::optional_string (p, "account");
		if (!account_text.empty ())
		{
			if (start.decode_account (account_text))
				return command_result::error ("Bad account number");
		}

		uint64_t modified_since = 0;
		auto modified_since_text = params::optional_string (p, "modified_since");
		if (!modified_since_text.empty ())
		{
			try
			{
				modified_since = std::stoull (modified_since_text);
			}
			catch (...)
			{
				return command_result::error ("Invalid timestamp");
			}
		}

		bool const sorting = params::optional_bool (p, "sorting", false);
		bool const representative = params::optional_bool (p, "representative", false);
		bool const weight = params::optional_bool (p, "weight", false);
		bool const pending_flag = params::optional_bool (p, "pending", false);
		bool const receivable = params::optional_bool (p, "receivable", pending_flag);

		boost::json::object accounts;
		auto transaction = ctx.node.ledger.tx_begin_read ();

		if (!sorting)
		{
			for (auto i = ctx.node.store.account.begin (transaction, start),
					  n = ctx.node.store.account.end (transaction);
				 i != n && accounts.size () < count;
				 ++i)
			{
				nano::account_info const & info = i->second;
				if (info.modified >= modified_since && (receivable || info.balance.number () >= threshold.number ()))
				{
					nano::account const & account = i->first;
					boost::json::object response_a;
					if (receivable)
					{
						auto account_receivable = ctx.node.ledger.account_receivable (transaction, account);
						if (info.balance.number () + account_receivable < threshold.number ())
						{
							continue;
						}
						response_a["pending"] = account_receivable.convert_to<std::string> ();
						response_a["receivable"] = account_receivable.convert_to<std::string> ();
					}
					response_a["frontier"] = info.head.to_string ();
					response_a["open_block"] = info.open_block.to_string ();
					response_a["representative_block"] = ctx.node.ledger.representative_block (transaction, info.head).to_string ();
					std::string balance = nano::uint128_union (info.balance).to_string_dec ();
					response_a["balance"] = balance;
					response_a["modified_timestamp"] = std::to_string (info.modified);
					response_a["block_count"] = std::to_string (info.block_count);
					if (representative)
					{
						response_a["representative"] = info.representative.to_account ();
					}
					if (weight)
					{
						auto account_weight = ctx.node.ledger.weight_exact (transaction, account);
						response_a["weight"] = account_weight.convert_to<std::string> ();
					}
					accounts[account.to_account ()] = std::move (response_a);
				}
			}
		}
		else
		{
			std::vector<std::pair<nano::uint128_union, nano::account>> ledger_l;
			for (auto i = ctx.node.store.account.begin (transaction, start),
					  n = ctx.node.store.account.end (transaction);
				 i != n;
				 ++i)
			{
				nano::account_info const & info = i->second;
				nano::uint128_union balance (info.balance);
				if (info.modified >= modified_since)
				{
					ledger_l.emplace_back (balance, i->first);
				}
			}
			std::sort (ledger_l.begin (), ledger_l.end ());
			std::reverse (ledger_l.begin (), ledger_l.end ());

			nano::account_info info;
			for (auto i = ledger_l.begin (), n = ledger_l.end (); i != n && accounts.size () < count; ++i)
			{
				ctx.node.store.account.get (transaction, i->second, info);
				if (receivable || info.balance.number () >= threshold.number ())
				{
					nano::account const & account = i->second;
					boost::json::object response_a;
					if (receivable)
					{
						auto account_receivable = ctx.node.ledger.account_receivable (transaction, account);
						if (info.balance.number () + account_receivable < threshold.number ())
						{
							continue;
						}
						response_a["pending"] = account_receivable.convert_to<std::string> ();
						response_a["receivable"] = account_receivable.convert_to<std::string> ();
					}
					response_a["frontier"] = info.head.to_string ();
					response_a["open_block"] = info.open_block.to_string ();
					response_a["representative_block"] = ctx.node.ledger.representative_block (transaction, info.head).to_string ();
					std::string balance = i->first.to_string_dec ();
					response_a["balance"] = balance;
					response_a["modified_timestamp"] = std::to_string (info.modified);
					response_a["block_count"] = std::to_string (info.block_count);
					if (representative)
					{
						response_a["representative"] = info.representative.to_account ();
					}
					if (weight)
					{
						auto account_weight = ctx.node.ledger.weight_exact (transaction, account);
						response_a["weight"] = account_weight.convert_to<std::string> ();
					}
					accounts[account.to_account ()] = std::move (response_a);
				}
			}
		}

		boost::json::object result;
		result["accounts"] = std::move (accounts);
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (ledger_command);

class unopened_command final : public sync_command
{
	std::string_view name () const override
	{
		return "unopened";
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();

		auto count = params::optional_count (p);
		auto threshold = params::optional_threshold (p);

		nano::account start{ 1 }; // exclude burn account by default
		auto account_text = params::optional_string (p, "account");
		if (!account_text.empty ())
		{
			if (start.decode_account (account_text))
				return command_result::error ("Bad account number");
		}

		auto transaction = ctx.node.store.tx_begin_read ();
		auto iterator = ctx.node.store.pending.begin (transaction, nano::pending_key (start, 0));
		auto end = ctx.node.store.pending.end (transaction);
		nano::account current_account = start;
		nano::uint128_t current_account_sum{ 0 };
		boost::json::object accounts;
		while (iterator != end && accounts.size () < count)
		{
			nano::pending_key key{ iterator->first };
			nano::account account{ key.account };
			nano::pending_info info{ iterator->second };
			if (ctx.node.store.account.exists (transaction, account))
			{
				if (account.number () == std::numeric_limits<nano::uint256_t>::max ())
				{
					break;
				}
				// Skip existing accounts
				iterator = ctx.node.store.pending.begin (transaction, nano::pending_key (nano::account (nano::inc_sat (account.number ())), 0));
			}
			else
			{
				if (account != current_account)
				{
					if (current_account_sum > 0)
					{
						if (current_account_sum >= threshold.number ())
						{
							accounts[current_account.to_account ()] = current_account_sum.convert_to<std::string> ();
						}
						current_account_sum = 0;
					}
					current_account = account;
				}
				current_account_sum += info.amount.number ();
				++iterator;
			}
		}
		// last one after iterator reaches end
		if (accounts.size () < count && current_account_sum > 0 && current_account_sum >= threshold.number ())
		{
			accounts[current_account.to_account ()] = current_account_sum.convert_to<std::string> ();
		}

		boost::json::object result;
		result["accounts"] = std::move (accounts);
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (unopened_command);
