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
bool block_confirmed (nano::node & node, nano::secure::transaction & transaction, nano::block_hash const & hash, bool include_active, bool include_only_confirmed)
{
	if (include_active && !include_only_confirmed)
	{
		return true;
	}
	else if (node.ledger.cemented.block_exists_or_pruned (transaction, hash))
	{
		return true;
	}
	else if (!include_only_confirmed)
	{
		auto block = node.ledger.any.block_get (transaction, hash);
		return (block != nullptr && !node.active.active (*block));
	}
	return false;
}

char const * epoch_as_string (nano::epoch epoch)
{
	switch (epoch)
	{
		case nano::epoch::epoch_2:
			return "2";
		case nano::epoch::epoch_1:
			return "1";
		default:
			return "0";
	}
}
}

class receivable_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "receivable";
	}

	command_result handle (request_context const & ctx) override
	{
		return do_receivable (ctx, false);
	}

protected:
	command_result do_receivable (request_context const & ctx, bool deprecated)
	{
		auto & p = ctx.params ();

		auto account_r = params::required_account (p);
		if (!account_r)
			return command_result::error (account_r.error_message ());
		auto account = *account_r;

		auto count = params::optional_count (p);
		auto offset = params::optional_uint64 (p, "offset", 0);
		auto threshold = params::optional_threshold (p);
		bool const source = params::optional_bool (p, "source", false);
		bool const min_version = params::optional_bool (p, "min_version", false);
		bool const include_active = params::optional_bool (p, "include_active", false);
		bool const include_only_confirmed = params::optional_bool (p, "include_only_confirmed", true);
		bool const sorting = params::optional_bool (p, "sorting", false);
		auto simple = threshold.is_zero () && !source && !min_version && !sorting;
		bool const should_sort = sorting && !simple;

		auto offset_counter = offset;
		auto transaction = ctx.node.ledger.tx_begin_read ();

		// For sorting, we collect all matches first
		std::vector<std::pair<std::string, boost::json::object>> hash_ptree_pairs;
		std::vector<std::pair<std::string, nano::uint128_t>> hash_amount_pairs;
		boost::json::object blocks_obj;
		boost::json::array blocks_arr;

		for (auto i = ctx.node.store.pending.begin (transaction, nano::pending_key (account, 0)),
				  n = ctx.node.store.pending.end (transaction);
			 i != n && nano::pending_key (i->first).account == account && (should_sort || (simple ? blocks_arr.size () < count : blocks_obj.size () < count));
			 ++i)
		{
			nano::pending_key const & key (i->first);
			if (block_confirmed (ctx.node, transaction, key.hash, include_active, include_only_confirmed))
			{
				if (!should_sort && offset_counter > 0)
				{
					--offset_counter;
					continue;
				}

				if (simple)
				{
					blocks_arr.emplace_back (key.hash.to_string ());
				}
				else
				{
					nano::pending_info const & info (i->second);
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
							if (min_version)
							{
								pending_tree["min_version"] = std::string (epoch_as_string (info.epoch));
							}

							if (should_sort)
							{
								hash_ptree_pairs.emplace_back (key.hash.to_string (), std::move (pending_tree));
							}
							else
							{
								blocks_obj[key.hash.to_string ()] = std::move (pending_tree);
							}
						}
						else
						{
							if (should_sort)
							{
								hash_amount_pairs.emplace_back (key.hash.to_string (), info.amount.number ());
							}
							else
							{
								blocks_obj[key.hash.to_string ()] = info.amount.number ().convert_to<std::string> ();
							}
						}
					}
				}
			}
		}

		if (should_sort)
		{
			if (source || min_version)
			{
				std::stable_sort (hash_ptree_pairs.begin (), hash_ptree_pairs.end (), [] (auto const & lhs, auto const & rhs) {
					// Sort by amount descending
					nano::uint128_t lhs_amount, rhs_amount;
					auto lhs_str = lhs.second.at ("amount").as_string ();
					auto rhs_str = rhs.second.at ("amount").as_string ();
					lhs_amount = nano::uint128_t (std::string (lhs_str));
					rhs_amount = nano::uint128_t (std::string (rhs_str));
					return lhs_amount > rhs_amount;
				});
				for (auto i = offset, j = offset + count; i < hash_ptree_pairs.size () && i < j; ++i)
				{
					blocks_obj[hash_ptree_pairs[i].first] = hash_ptree_pairs[i].second;
				}
			}
			else
			{
				std::stable_sort (hash_amount_pairs.begin (), hash_amount_pairs.end (), [] (auto const & lhs, auto const & rhs) {
					return lhs.second > rhs.second;
				});
				for (auto i = offset, j = offset + count; i < hash_amount_pairs.size () && i < j; ++i)
				{
					blocks_obj[hash_amount_pairs[i].first] = hash_amount_pairs[i].second.convert_to<std::string> ();
				}
			}
		}

		boost::json::object result;
		if (deprecated)
		{
			result["deprecated"] = "1";
		}
		if (simple)
		{
			result["blocks"] = std::move (blocks_arr);
		}
		else
		{
			result["blocks"] = std::move (blocks_obj);
		}
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (receivable_command);

class receivable_exists_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "receivable_exists";
	}

	command_result handle (request_context const & ctx) override
	{
		return do_receivable_exists (ctx, false);
	}

protected:
	command_result do_receivable_exists (request_context const & ctx, bool deprecated)
	{
		auto & p = ctx.params ();

		auto hash_r = params::required_hash (p);
		if (!hash_r)
			return command_result::error (hash_r.error_message ());
		auto hash = *hash_r;

		bool const include_active = params::optional_bool (p, "include_active", false);
		bool const include_only_confirmed = params::optional_bool (p, "include_only_confirmed", true);

		auto transaction = ctx.node.ledger.tx_begin_read ();
		auto block = ctx.node.ledger.any.block_get (transaction, hash);
		if (block != nullptr)
		{
			auto exists = false;
			if (block->is_send ())
			{
				exists = ctx.node.ledger.any.pending_get (transaction, nano::pending_key{ block->destination (), hash }).has_value ();
			}
			exists = exists && block_confirmed (ctx.node, transaction, block->hash (), include_active, include_only_confirmed);

			boost::json::object result;
			if (deprecated)
			{
				result["deprecated"] = "1";
			}
			result["exists"] = exists ? "1" : "0";
			return command_result::ok (std::move (result));
		}
		else
		{
			return command_result::error ("Block not found");
		}
	}
};
REGISTER_RPC_COMMAND (receivable_exists_command);

class pending_command final : public receivable_command
{
	std::string_view name () const override
	{
		return "pending";
	}
	command_result handle (request_context const & ctx) override
	{
		return do_receivable (ctx, true);
	}
};
REGISTER_RPC_COMMAND (pending_command);

class pending_exists_command final : public receivable_exists_command
{
	std::string_view name () const override
	{
		return "pending_exists";
	}
	command_result handle (request_context const & ctx) override
	{
		return do_receivable_exists (ctx, true);
	}
};
REGISTER_RPC_COMMAND (pending_exists_command);
