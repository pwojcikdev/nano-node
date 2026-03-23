#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/common.hpp>

using namespace nano::rpc_api;

// account_create: Create a new account in a wallet (async - uses workers)
class account_create_cmd final : public command
{
	std::string_view name () const override
	{
		return "account_create";
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

		bool const generate_work = params::optional_bool (ctx.params (), "work", true);
		auto index_str = params::optional_string (ctx.params (), "index");

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;

		ctx.node.workers.post ([wallet_ptr, generate_work, index_str, respond, respond_error] () {
			try
			{
				nano::account new_key;
				if (!index_str.empty ())
				{
					uint64_t index;
					try
					{
						std::size_t end;
						index = std::stoull (index_str, &end);
						if (end != index_str.size () || index > static_cast<uint64_t> (std::numeric_limits<uint32_t>::max ()))
						{
							respond_error ("Invalid index");
							return;
						}
					}
					catch (...)
					{
						respond_error ("Invalid index");
						return;
					}
					new_key = wallet_ptr->deterministic_insert (static_cast<uint32_t> (index), generate_work);
				}
				else
				{
					new_key = wallet_ptr->deterministic_insert (generate_work);
				}

				if (!new_key.is_zero ())
				{
					respond (boost::json::object{ { "account", new_key.to_account () } });
				}
				else
				{
					respond_error ("Wallet locked");
				}
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (account_create_cmd);

// account_remove: Remove an account from a wallet (async)
class account_remove_cmd final : public command
{
	std::string_view name () const override
	{
		return "account_remove";
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

		auto account = params::required_account (ctx.params ());
		if (!account)
		{
			ctx.respond_error (account.error_message ());
			return;
		}

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;
		auto account_val = *account;

		ctx.node.workers.post ([wallet_ptr, account_val, respond, respond_error] () {
			try
			{
				if (wallet_ptr->is_locked ())
				{
					respond_error ("Wallet locked");
					return;
				}
				if (!wallet_ptr->exists (account_val))
				{
					respond_error ("Account not found in wallet");
					return;
				}
				wallet_ptr->remove_account (account_val);
				respond (boost::json::object{ { "removed", "1" } });
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (account_remove_cmd);

// account_move: Move accounts between wallets (async)
class account_move_cmd final : public command
{
	std::string_view name () const override
	{
		return "account_move";
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

		auto source_str = params::required_string (ctx.params (), "source");
		if (!source_str)
		{
			ctx.respond_error (source_str.error_message ());
			return;
		}

		auto accounts_arr = params::required_string_array (ctx.params (), "accounts");
		if (!accounts_arr)
		{
			ctx.respond_error (accounts_arr.error_message ());
			return;
		}

		// Parse source wallet id
		nano::wallet_id source_id;
		if (source_id.decode_hex (*source_str))
		{
			ctx.respond_error ("Bad source");
			return;
		}

		// Parse accounts
		std::vector<nano::public_key> accounts;
		for (auto const & acc_str : *accounts_arr)
		{
			nano::account acc;
			if (acc.decode_account (acc_str))
			{
				ctx.respond_error ("Bad account number");
				return;
			}
			accounts.push_back (acc);
		}

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;
		auto & node = ctx.node;

		node.workers.post ([&node, wallet_ptr, source_id, accounts = std::move (accounts), respond, respond_error] () {
			try
			{
				auto existing = node.wallets.items.find (source_id);
				if (existing != node.wallets.items.end ())
				{
					auto source_wallet = existing->second;
					auto error = wallet_ptr->move_accounts (*source_wallet, accounts);
					respond (boost::json::object{ { "moved", error ? "0" : "1" } });
				}
				else
				{
					respond_error ("Source not found");
				}
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (account_move_cmd);

// accounts_create: Create multiple accounts in a wallet (async)
class accounts_create_cmd final : public command
{
	std::string_view name () const override
	{
		return "accounts_create";
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

		auto count = params::required_count (ctx.params ());
		if (!count)
		{
			ctx.respond_error (count.error_message ());
			return;
		}

		bool const generate_work = params::optional_bool (ctx.params (), "work", false);

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;
		auto count_val = *count;

		ctx.node.workers.post ([wallet_ptr, count_val, generate_work, respond, respond_error] () {
			try
			{
				boost::json::array accounts;
				for (uint64_t i = 0; accounts.size () < count_val; ++i)
				{
					nano::account new_key (wallet_ptr->deterministic_insert (generate_work));
					if (!new_key.is_zero ())
					{
						accounts.push_back (boost::json::value (new_key.to_account ()));
					}
				}
				respond (boost::json::object{ { "accounts", accounts } });
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (accounts_create_cmd);

// wallet_add: Add a private key to a wallet (async)
class wallet_add_cmd final : public command
{
	std::string_view name () const override
	{
		return "wallet_add";
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

		auto key_str = params::required_string (ctx.params (), "key");
		if (!key_str)
		{
			ctx.respond_error (key_str.error_message ());
			return;
		}

		nano::raw_key key;
		if (key.decode_hex (*key_str))
		{
			ctx.respond_error ("Bad private key");
			return;
		}

		bool const generate_work = params::optional_bool (ctx.params (), "work", true);

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;

		ctx.node.workers.post ([wallet_ptr, key, generate_work, respond, respond_error] () {
			try
			{
				auto pub = wallet_ptr->insert_adhoc (key, generate_work);
				if (!pub.is_zero ())
				{
					respond (boost::json::object{ { "account", pub.to_account () } });
				}
				else
				{
					respond_error ("Wallet locked");
				}
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (wallet_add_cmd);

// wallet_add_watch: Add watch-only accounts to a wallet (async)
class wallet_add_watch_cmd final : public command
{
	std::string_view name () const override
	{
		return "wallet_add_watch";
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

		auto accounts_arr = params::required_string_array (ctx.params (), "accounts");
		if (!accounts_arr)
		{
			ctx.respond_error (accounts_arr.error_message ());
			return;
		}

		// Parse all accounts first
		std::vector<nano::account> accounts;
		for (auto const & acc_str : *accounts_arr)
		{
			nano::account acc;
			if (acc.decode_account (acc_str))
			{
				ctx.respond_error ("Bad account number");
				return;
			}
			accounts.push_back (acc);
		}

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;

		ctx.node.workers.post ([wallet_ptr, accounts = std::move (accounts), respond, respond_error] () {
			try
			{
				if (wallet_ptr->is_locked ())
				{
					respond_error ("Wallet locked");
					return;
				}
				for (auto const & account : accounts)
				{
					if (wallet_ptr->insert_watch (account))
					{
						respond_error ("Bad public key");
						return;
					}
				}
				respond (boost::json::object{ { "success", "" } });
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (wallet_add_watch_cmd);

// wallet_change_seed: Change the seed of a wallet (async)
class wallet_change_seed_cmd final : public command
{
	std::string_view name () const override
	{
		return "wallet_change_seed";
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

		auto seed_str = params::required_string (ctx.params (), "seed");
		if (!seed_str)
		{
			ctx.respond_error (seed_str.error_message ());
			return;
		}

		nano::raw_key seed;
		if (seed.decode_hex (*seed_str))
		{
			ctx.respond_error ("Bad seed");
			return;
		}

		auto count_val = static_cast<uint32_t> (params::optional_count (ctx.params (), 0));

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;

		ctx.node.workers.post ([wallet_ptr, seed, count_val, respond, respond_error] () {
			try
			{
				if (wallet_ptr->is_locked ())
				{
					respond_error ("Wallet locked");
					return;
				}
				nano::public_key account (wallet_ptr->change_seed (seed, count_val));
				auto index = wallet_ptr->get_deterministic_index ();
				respond (boost::json::object{
				{ "success", "" },
				{ "last_restored_account", account.to_account () },
				{ "restored_count", std::to_string (index) },
				});
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (wallet_change_seed_cmd);

// wallet_create: Create a new wallet (async)
class wallet_create_cmd final : public command
{
	std::string_view name () const override
	{
		return "wallet_create";
	}
	bool requires_control () const override
	{
		return true;
	}

	void execute (request_context & ctx) override
	{
		auto seed_str = params::optional_string (ctx.params (), "seed");
		nano::raw_key seed;
		bool has_seed = false;
		if (!seed_str.empty ())
		{
			if (seed.decode_hex (seed_str))
			{
				ctx.respond_error ("Bad seed");
				return;
			}
			has_seed = true;
		}

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto & node = ctx.node;

		node.workers.post ([&node, seed, has_seed, respond, respond_error] () {
			try
			{
				auto wallet_id = nano::random_wallet_id ();
				auto wallet = node.wallets.create (wallet_id);
				auto existing = node.wallets.items.find (wallet_id);
				if (existing == node.wallets.items.end ())
				{
					respond_error ("Failed to create wallet. Increase lmdb_max_dbs in node config");
					return;
				}

				boost::json::object result;
				result["wallet"] = wallet_id.to_string ();

				if (has_seed)
				{
					nano::public_key account (wallet->change_seed (seed));
					result["last_restored_account"] = account.to_account ();
					auto index = wallet->get_deterministic_index ();
					result["restored_count"] = std::to_string (index);
				}

				respond (result);
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (wallet_create_cmd);

// wallet_destroy: Destroy a wallet (async)
class wallet_destroy_cmd final : public command
{
	std::string_view name () const override
	{
		return "wallet_destroy";
	}
	bool requires_control () const override
	{
		return true;
	}

	void execute (request_context & ctx) override
	{
		auto wallet_str = params::required_string (ctx.params (), "wallet");
		if (!wallet_str)
		{
			ctx.respond_error (wallet_str.error_message ());
			return;
		}

		nano::wallet_id wallet_id;
		if (wallet_id.decode_hex (*wallet_str))
		{
			ctx.respond_error ("Bad wallet number");
			return;
		}

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto & node = ctx.node;

		node.workers.post ([&node, wallet_id, respond, respond_error] () {
			try
			{
				auto existing = node.wallets.items.find (wallet_id);
				if (existing != node.wallets.items.end ())
				{
					node.wallets.destroy (wallet_id);
					bool destroyed = node.wallets.items.find (wallet_id) == node.wallets.items.end ();
					respond (boost::json::object{ { "destroyed", destroyed ? "1" : "0" } });
				}
				else
				{
					respond_error ("Wallet not found");
				}
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (wallet_destroy_cmd);
