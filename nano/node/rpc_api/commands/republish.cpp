#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/transport/traffic_type.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

using namespace nano::rpc_api;

class republish_command final : public sync_command
{
	std::string_view name () const override
	{
		return "republish";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();

		auto count = params::optional_count (p, 1024);

		uint64_t sources = 0;
		auto sources_text = params::optional_string (p, "sources");
		if (!sources_text.empty ())
		{
			try
			{
				sources = std::stoull (sources_text);
			}
			catch (...)
			{
				return command_result::error ("Invalid sources number");
			}
		}

		uint64_t destinations = 0;
		auto destinations_text = params::optional_string (p, "destinations");
		if (!destinations_text.empty ())
		{
			try
			{
				destinations = std::stoull (destinations_text);
			}
			catch (...)
			{
				return command_result::error ("Invalid destinations number");
			}
		}

		auto hash_r = params::required_hash (p);
		if (!hash_r)
			return command_result::error (hash_r.error_message ());
		auto hash = *hash_r;

		boost::json::array blocks;
		auto transaction = ctx.node.ledger.tx_begin_read ();
		auto block = ctx.node.ledger.any.block_get (transaction, hash);
		if (block != nullptr)
		{
			std::deque<std::shared_ptr<nano::block>> republish_bundle;
			for (uint64_t i = 0; !hash.is_zero () && i < count; ++i)
			{
				block = ctx.node.ledger.any.block_get (transaction, hash);
				if (sources != 0) // Republish source chain
				{
					nano::block_hash source = block->source_field ().value_or (block->link_field ().value_or (0).as_block_hash ());
					auto block_a = ctx.node.ledger.any.block_get (transaction, source);
					std::vector<nano::block_hash> hashes;
					while (block_a != nullptr && hashes.size () < sources)
					{
						hashes.push_back (source);
						source = block_a->previous ();
						block_a = ctx.node.ledger.any.block_get (transaction, source);
					}
					std::reverse (hashes.begin (), hashes.end ());
					for (auto & hash_l : hashes)
					{
						block_a = ctx.node.ledger.any.block_get (transaction, hash_l);
						republish_bundle.push_back (std::move (block_a));
						blocks.emplace_back (hash_l.to_string ());
					}
				}
				republish_bundle.push_back (std::move (block)); // Republish block
				blocks.emplace_back (hash.to_string ());
				if (destinations != 0) // Republish destination chain
				{
					auto block_b = ctx.node.ledger.any.block_get (transaction, hash);
					auto destination = block_b->is_send () ? block_b->destination () : nano::account (0);
					if (!destination.is_zero ())
					{
						if (!ctx.node.ledger.any.pending_get (transaction, nano::pending_key{ destination, hash }))
						{
							nano::block_hash previous (ctx.node.ledger.any.account_head (transaction, destination));
							auto block_d = ctx.node.ledger.any.block_get (transaction, previous);
							nano::block_hash source;
							std::vector<nano::block_hash> hashes;
							while (block_d != nullptr && hash != source)
							{
								hashes.push_back (previous);
								source = block_d->source_field ().value_or (block_d->is_send () ? nano::block_hash (0) : block_d->link_field ().value_or (0).as_block_hash ());
								previous = block_d->previous ();
								block_d = ctx.node.ledger.any.block_get (transaction, previous);
							}
							std::reverse (hashes.begin (), hashes.end ());
							if (hashes.size () > destinations)
							{
								hashes.resize (destinations);
							}
							for (auto & hash_l : hashes)
							{
								block_d = ctx.node.ledger.any.block_get (transaction, hash_l);
								republish_bundle.push_back (std::move (block_d));
								blocks.emplace_back (hash_l.to_string ());
							}
						}
					}
				}
				hash = ctx.node.ledger.any.block_successor (transaction, hash).value_or (0);
			}
			ctx.node.network.flood_block_many (std::move (republish_bundle), nano::transport::traffic_type::block_broadcast_rpc, std::chrono::milliseconds (25));

			boost::json::object result;
			result["success"] = ""; // obsolete
			result["blocks"] = std::move (blocks);
			return command_result::ok (std::move (result));
		}
		else
		{
			return command_result::error ("Block not found");
		}
	}
};
REGISTER_RPC_COMMAND (republish_command);
