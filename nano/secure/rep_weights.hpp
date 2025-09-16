#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/utility.hpp>
#include <nano/secure/fwd.hpp>

#include <deque>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <variant>

namespace nano
{
class rep_weights;

class rep_weights_updates
{
public:
	void apply (nano::secure::write_transaction &, nano::rep_weights &) const;
	void clear ();

public:
	/* Adds or subtracts weight to the representative */
	void add (nano::account const & rep, nano::uint128_t const & amount_add);
	void sub (nano::account const & rep, nano::uint128_t const & amount_sub);

	/* Move weight from one representative to another */
	void move (nano::account const & source_rep, nano::account const & dest_rep, nano::uint128_t const & amount);

	/* Move weight from one representative to another while adding or subtracting the weight */
	void move_add_sub (nano::account const & source_rep, nano::uint128_t const & amount_source, nano::account const & dest_rep, nano::uint128_t const & amount_dest);

private:
	struct op_add
	{
		nano::account rep;
		nano::uint128_t amount_add;
	};
	struct op_sub
	{
		nano::account rep;
		nano::uint128_t amount_sub;
	};
	struct op_move
	{
		nano::account source_rep;
		nano::account dest_rep;
		nano::uint128_t amount;
	};
	struct op_move_add_sub
	{
		nano::account source_rep;
		nano::uint128_t amount_source;
		nano::account dest_rep;
		nano::uint128_t amount_dest;
	};

	using update_variant = std::variant<op_add, op_sub, op_move, op_move_add_sub>;
	std::deque<update_variant> updates;
};

class rep_weights
{
public:
	explicit rep_weights (nano::store::rep_weight &, nano::uint128_t min_weight = 0);

	/* Adds or subtracts weight to the representative */
	void add (secure::write_transaction &, nano::account const & rep, nano::uint128_t const & amount_add);
	void sub (secure::write_transaction &, nano::account const & rep, nano::uint128_t const & amount_sub);

	/* Move weight from one representative to another */
	void move (secure::write_transaction &, nano::account const & source_rep, nano::account const & dest_rep, nano::uint128_t const & amount);

	/* Move weight from one representative to another while adding or subtracting the weight */
	void move_add_sub (secure::write_transaction &, nano::account const & source_rep, nano::uint128_t const & amount_source, nano::account const & dest_rep, nano::uint128_t const & amount_dest);

	/* Only use this method when loading rep weights from the database table */
	void put (nano::account const & rep, nano::uint128_t const & weight);
	void put_unused (nano::uint128_t const & weight);
	void append_from (rep_weights const & other);

	nano::uint128_t get (nano::account const & rep) const;
	std::unordered_map<nano::account, nano::uint128_t> get_rep_amounts () const;

	size_t size () const;
	nano::container_info container_info () const;
	bool empty () const;

	nano::uint128_t get_weight_committed () const;
	nano::uint128_t get_weight_unused () const;

	void verify_consistency (nano::uint128_t burn_balance) const;

private:
	nano::store::rep_weight & rep_weight_store;
	nano::uint128_t const min_weight;

	mutable std::shared_mutex mutex;
	std::unordered_map<nano::account, nano::uint128_t> rep_amounts;

	// Used for consistency checking, use higher precision types to detect overflows
	nano::uint256_t weight_committed{ 0 };
	nano::uint256_t weight_unused{ 0 };

private:
	void put_cache (nano::account const & rep, nano::uint128_union const & weight);
	void put_store (store::write_transaction const &, nano::account const & rep, nano::uint128_t const & previous_weight, nano::uint128_t const & new_weight);
	nano::uint128_t get_impl (nano::account const & rep) const;
};
}
