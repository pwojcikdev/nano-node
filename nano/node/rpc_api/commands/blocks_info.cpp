#include <nano/lib/block_sideband.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger/successor.hpp>

#include <boost/json/parse.hpp>

using namespace nano::rpc_api;

// blocks: Retrieve contents for a list of block hashes
class blocks_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "blocks";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		bool const json_block = params::optional_bool (ctx.params (), "json_block", false);

		auto hashes_arr = params::required_string_array (ctx.params (), "hashes");
		if (!hashes_arr)
			return command_result::error (hashes_arr.error_message ());

		boost::json::object blocks;
		auto transaction = ctx.node.ledger.tx_begin_read ();

		for (auto const & hash_text : *hashes_arr)
		{
			nano::block_hash hash;
			if (hash.decode_hex (hash_text))
				return command_result::error ("Bad hash number");

			auto block = ctx.node.ledger.any.block_get (transaction, hash);
			if (block != nullptr)
			{
				if (json_block)
				{
					// Serialize block as JSON object
					std::string contents;
					block->serialize_json (contents);
					auto parsed = boost::json::parse (contents);
					blocks[hash_text] = parsed;
				}
				else
				{
					std::string contents;
					block->serialize_json (contents);
					blocks[hash_text] = contents;
				}
			}
			else
			{
				return command_result::error ("Block not found");
			}
		}
		return command_result::ok ({ { "blocks", blocks } });
	}
};
REGISTER_RPC_COMMAND (blocks_cmd);

// blocks_info: Detailed info for a list of block hashes
class blocks_info_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "blocks_info";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		bool const pending_flag = params::optional_bool (ctx.params (), "pending", false);
		bool const receivable = params::optional_bool (ctx.params (), "receivable", pending_flag);
		bool const receive_hash = params::optional_bool (ctx.params (), "receive_hash", false);
		bool const source = params::optional_bool (ctx.params (), "source", false);
		bool const json_block = params::optional_bool (ctx.params (), "json_block", false);
		bool const include_linked_account = params::optional_bool (ctx.params (), "include_linked_account", false);
		bool const include_not_found = params::optional_bool (ctx.params (), "include_not_found", false);

		auto hashes_arr = params::required_string_array (ctx.params (), "hashes");
		if (!hashes_arr)
			return command_result::error (hashes_arr.error_message ());

		boost::json::object blocks;
		boost::json::array blocks_not_found;
		auto transaction = ctx.node.ledger.tx_begin_read ();

		for (auto const & hash_text : *hashes_arr)
		{
			nano::block_hash hash;
			if (hash.decode_hex (hash_text))
				return command_result::error ("Bad hash number");

			auto block = ctx.node.ledger.any.block_get (transaction, hash);
			if (block != nullptr)
			{
				boost::json::object entry;
				auto account = block->account ();
				entry["block_account"] = account.to_account ();

				if (include_linked_account)
				{
					auto linked_account = ctx.node.ledger.linked_account (transaction, *block);
					if (linked_account.has_value ())
					{
						entry["linked_account"] = linked_account.value ().to_account ();
					}
					else
					{
						entry["linked_account"] = "0";
					}
				}

				auto amount = ctx.node.ledger.any.block_amount (transaction, hash);
				if (amount)
				{
					entry["amount"] = amount.value ().number ().convert_to<std::string> ();
				}
				auto balance = block->balance ();
				entry["balance"] = balance.number ().convert_to<std::string> ();
				entry["height"] = std::to_string (block->sideband ().height);
				entry["local_timestamp"] = std::to_string (block->sideband ().timestamp);
				entry["successor"] = ctx.node.store.successor.get (transaction, hash).value_or (nano::block_hash{ 0 }).to_string ();
				auto confirmed = ctx.node.ledger.cemented.block_exists_or_pruned (transaction, hash);
				entry["confirmed"] = confirmed ? "true" : "false";

				if (json_block)
				{
					std::string contents;
					block->serialize_json (contents);
					auto parsed = boost::json::parse (contents);
					entry["contents"] = parsed;
				}
				else
				{
					std::string contents;
					block->serialize_json (contents);
					entry["contents"] = contents;
				}

				if (block->type () == nano::block_type::state)
				{
					auto subtype = nano::state_subtype (block->sideband ().details);
					entry["subtype"] = subtype;
				}

				if (receivable || receive_hash)
				{
					if (!block->is_send ())
					{
						if (receivable)
						{
							entry["pending"] = "0";
							entry["receivable"] = "0";
						}
						if (receive_hash)
						{
							entry["receive_hash"] = nano::block_hash (0).to_string ();
						}
					}
					else if (ctx.node.ledger.any.pending_get (transaction, nano::pending_key{ block->destination (), hash }))
					{
						if (receivable)
						{
							entry["pending"] = "1";
							entry["receivable"] = "1";
						}
						if (receive_hash)
						{
							entry["receive_hash"] = nano::block_hash (0).to_string ();
						}
					}
					else
					{
						if (receivable)
						{
							entry["pending"] = "0";
							entry["receivable"] = "0";
						}
						if (receive_hash)
						{
							std::shared_ptr<nano::block> receive_block = ctx.node.ledger.find_receive_block_by_send_hash (transaction, block->destination (), hash);
							std::string recv_hash = receive_block ? receive_block->hash ().to_string () : nano::block_hash (0).to_string ();
							entry["receive_hash"] = recv_hash;
						}
					}
				}
				if (source)
				{
					if (!block->is_receive () || !ctx.node.ledger.any.block_exists (transaction, block->source ()))
					{
						entry["source_account"] = "0";
					}
					else
					{
						auto block_a = ctx.node.ledger.any.block_get (transaction, block->source ());
						release_assert (block_a);
						entry["source_account"] = block_a->account ().to_account ();
					}
				}
				blocks[hash_text] = entry;
			}
			else if (include_not_found)
			{
				blocks_not_found.push_back (boost::json::value (hash_text));
			}
			else
			{
				return command_result::error ("Block not found");
			}
		}

		boost::json::object result;
		result["blocks"] = blocks;
		if (include_not_found)
		{
			result["blocks_not_found"] = blocks_not_found;
		}
		return command_result::ok (result);
	}
};
REGISTER_RPC_COMMAND (blocks_info_cmd);
