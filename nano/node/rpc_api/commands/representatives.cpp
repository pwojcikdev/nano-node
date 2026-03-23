#include <nano/node/node.hpp>
#include <nano/node/online_reps.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>

using namespace nano::rpc_api;

class representatives_command final : public sync_command
{
	std::string_view name () const override
	{
		return "representatives";
	}
	command_result handle (request_context const & ctx) override
	{
		auto count = params::optional_count (ctx.params ());
		bool sorting = params::optional_bool (ctx.params (), "sorting");
		auto rep_amounts = ctx.node.ledger.rep_weights.get_rep_amounts ();
		boost::json::object representatives;
		if (!sorting)
		{
			for (auto & [account, amount] : rep_amounts)
			{
				representatives[account.to_account ()] = amount.convert_to<std::string> ();
				if (representatives.size () > count)
					break;
			}
		}
		else
		{
			std::vector<std::pair<nano::uint128_t, std::string>> sorted;
			for (auto & [account, amount] : rep_amounts)
				sorted.emplace_back (amount, account.to_account ());
			std::sort (sorted.begin (), sorted.end (), std::greater<> ());
			for (auto const & [amount, account] : sorted)
			{
				if (representatives.size () >= count)
					break;
				representatives[account] = amount.convert_to<std::string> ();
			}
		}
		return command_result::ok ({ { "representatives", representatives } });
	}
};
REGISTER_RPC_COMMAND (representatives_command);

class representatives_online_command final : public sync_command
{
	std::string_view name () const override
	{
		return "representatives_online";
	}
	command_result handle (request_context const & ctx) override
	{
		bool weight_flag = params::optional_bool (ctx.params (), "weight");
		std::vector<nano::public_key> filter;
		auto it = ctx.params ().find ("accounts");
		if (it != ctx.params ().end () && it->value ().is_array ())
		{
			for (auto const & a : it->value ().as_array ())
			{
				nano::account acc;
				if (a.is_string () && !acc.decode_account (std::string (a.as_string ())))
					filter.push_back (acc);
				else
					return command_result::error ("Bad account number");
			}
		}

		auto reps = ctx.node.online_reps.list ();
		if (weight_flag)
		{
			boost::json::object representatives;
			for (auto & rep : reps)
			{
				if (!filter.empty ())
				{
					auto found = std::find (filter.begin (), filter.end (), rep);
					if (found == filter.end ())
						continue;
					filter.erase (found);
				}
				representatives[rep.to_account ()] = boost::json::object{
					{ "weight", ctx.node.ledger.weight (rep).convert_to<std::string> () },
				};
			}
			return command_result::ok ({ { "representatives", representatives } });
		}
		else
		{
			boost::json::array representatives;
			for (auto & rep : reps)
			{
				if (!filter.empty ())
				{
					auto found = std::find (filter.begin (), filter.end (), rep);
					if (found == filter.end ())
						continue;
					filter.erase (found);
				}
				representatives.push_back (boost::json::value (rep.to_account ()));
			}
			return command_result::ok ({ { "representatives", representatives } });
		}
	}
};
REGISTER_RPC_COMMAND (representatives_online_command);
