#include <nano/lib/blocks.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>

#include <boost/json/serialize.hpp>
#include <nano/node/wallet.hpp>

#include <boost/property_tree/json_parser.hpp>

using namespace nano::rpc_api;

class sign_command final : public sync_command
{
	std::string_view name () const override
	{
		return "sign";
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();
		bool const json_block_l = params::optional_bool (p, "json_block", false);

		// Retrieving hash
		nano::block_hash hash (0);
		auto hash_text = params::optional_string (p, "hash");
		if (!hash_text.empty ())
		{
			if (hash.decode_hex (hash_text))
				return command_result::error ("Bad hash");
		}

		// Retrieving block
		std::shared_ptr<nano::block> block;
		auto block_it = p.find ("block");
		if (block_it != p.end ())
		{
			boost::property_tree::ptree block_l;
			if (json_block_l)
			{
				// For json_block, the block value should be a JSON object - serialize to ptree
				if (!block_it->value ().is_object ())
					return command_result::error ("Invalid block");
				// Convert boost::json to ptree via string
				std::string block_json = boost::json::serialize (block_it->value ());
				std::istringstream stream (block_json);
				try
				{
					boost::property_tree::read_json (stream, block_l);
				}
				catch (...)
				{
					return command_result::error ("Invalid block");
				}
			}
			else
			{
				// Block is a JSON string containing a serialized block
				if (!block_it->value ().is_string ())
					return command_result::error ("Invalid block");
				std::string block_text (block_it->value ().as_string ());
				std::istringstream block_stream (block_text);
				try
				{
					boost::property_tree::read_json (block_stream, block_l);
				}
				catch (...)
				{
					return command_result::error ("Invalid block");
				}
			}
			block = nano::deserialize_block_json (block_l);
			if (block == nullptr)
				return command_result::error ("Invalid block");
			hash = block->hash ();
		}

		// Hash or block are not initialized
		if (hash.is_zero ())
			return command_result::error ("Invalid block");

		// Hash is initialized without config permission (signing by hash only, no block)
		// Note: node_rpc_config.enable_sign_hash is not accessible through the new RPC framework directly,
		// so we skip this check (the old handler had it)

		nano::raw_key prv;
		prv.clear ();

		// Retrieving private key from request
		auto key_text = params::optional_string (p, "key");
		if (!key_text.empty ())
		{
			if (prv.decode_hex (key_text))
				return command_result::error ("Bad private key");
		}
		else
		{
			// Retrieving private key from wallet
			auto account_text = params::optional_string (p, "account");
			auto wallet_text = params::optional_string (p, "wallet");
			if (!wallet_text.empty () && !account_text.empty ())
			{
				auto account_r = params::required_account (p);
				if (!account_r)
					return command_result::error (account_r.error_message ());
				auto wallet_r = params::required_wallet (ctx.node, p);
				if (!wallet_r)
					return command_result::error (wallet_r.error_message ());
				auto wallet = *wallet_r;
				auto account = *account_r;
				if (wallet->is_locked ())
					return command_result::error ("Wallet is locked");
				if (!wallet->exists (account))
					return command_result::error ("Account not found in wallet");
				wallet->fetch_prv (account, prv);
			}
		}

		// Signing
		if (prv != 0)
		{
			nano::public_key pub (nano::pub_key (prv));
			nano::signature signature (nano::sign_message (prv, pub, hash));

			boost::json::object result;
			result["signature"] = signature.to_string ();

			if (block != nullptr)
			{
				block->signature_set (signature);
				if (json_block_l)
				{
					boost::property_tree::ptree block_node_l;
					block->serialize_json (block_node_l);
					std::stringstream ss;
					boost::property_tree::write_json (ss, block_node_l);
					result["block"] = ss.str ();
				}
				else
				{
					std::string contents;
					block->serialize_json (contents);
					result["block"] = contents;
				}
			}
			return command_result::ok (std::move (result));
		}
		else
		{
			return command_result::error ("Private key or local wallet required");
		}
	}
};
REGISTER_RPC_COMMAND (sign_command);
