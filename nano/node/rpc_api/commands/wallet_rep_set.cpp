#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

using namespace nano::rpc_api;

// wallet_representative_set: Set default representative for a wallet (async)
// Optionally updates all existing wallet accounts to use the new representative
class wallet_representative_set_cmd final : public command
{
	std::string_view name () const override
	{
		return "wallet_representative_set";
	}
	bool requires_control () const override
	{
		return true;
	}

	void execute (request_context & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
		{
			ctx.respond_error (wallet.error_message ());
			return;
		}

		auto rep_str = params::required_string (ctx.params (), "representative");
		if (!rep_str)
		{
			ctx.respond_error (rep_str.error_message ());
			return;
		}

		nano::account representative;
		if (representative.decode_account (*rep_str))
		{
			ctx.respond_error ("Bad representative number");
			return;
		}

		bool update_existing_accounts = params::optional_bool (ctx.params (), "update_existing_accounts", false);

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;
		auto & node = ctx.node;

		node.workers.post ([&node, wallet_ptr, representative, update_existing_accounts, respond, respond_error] () {
			try
			{
				if (update_existing_accounts && wallet_ptr->is_locked ())
				{
					respond_error ("Wallet locked");
					return;
				}

				wallet_ptr->set_representative (representative);

				// Change representative for all wallet accounts if requested
				if (update_existing_accounts)
				{
					std::vector<nano::account> accounts;
					{
						auto block_transaction = node.ledger.tx_begin_read ();
						for (auto const & account : wallet_ptr->accounts ())
						{
							auto info = node.ledger.any.account_get (block_transaction, account);
							if (info)
							{
								if (info->representative != representative)
								{
									accounts.push_back (account);
								}
							}
						}
					}
					for (auto & account : accounts)
					{
						wallet_ptr->change_async (
						account, representative, [] (std::shared_ptr<nano::block> const &) {}, 0, false);
					}
				}

				respond (boost::json::object{ { "set", "1" } });
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (wallet_representative_set_cmd);
