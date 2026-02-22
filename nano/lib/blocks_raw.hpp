#pragma once

#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks_hashables.hpp>
#include <nano/lib/fwd.hpp>

#include <variant>

namespace nano
{
/*
 * raw_send_block
 */
struct raw_send_block
{
	nano::send_hashables hashables;
	nano::signature signature{};
	uint64_t work{};

	static constexpr nano::block_type block_type_v = nano::block_type::send;

	void serialize (nano::stream &) const;
	void deserialize (nano::stream &);
	nano::root root () const;
	bool operator== (raw_send_block const &) const = default;

	nano::amount balance_field () const;
	nano::account destination_field () const;
	nano::block_hash previous_field () const;
};

/*
 * raw_receive_block
 */
struct raw_receive_block
{
	nano::receive_hashables hashables;
	nano::signature signature{};
	uint64_t work{};

	static constexpr nano::block_type block_type_v = nano::block_type::receive;

	void serialize (nano::stream &) const;
	void deserialize (nano::stream &);
	nano::root root () const;
	bool operator== (raw_receive_block const &) const = default;

	nano::block_hash previous_field () const;
	nano::block_hash source_field () const;
};

/*
 * raw_open_block
 */
struct raw_open_block
{
	nano::open_hashables hashables;
	nano::signature signature{};
	uint64_t work{};

	static constexpr nano::block_type block_type_v = nano::block_type::open;

	void serialize (nano::stream &) const;
	void deserialize (nano::stream &);
	nano::root root () const;
	bool operator== (raw_open_block const &) const = default;

	nano::account account_field () const;
	nano::account representative_field () const;
	nano::block_hash source_field () const;
};

/*
 * raw_change_block
 */
struct raw_change_block
{
	nano::change_hashables hashables;
	nano::signature signature{};
	uint64_t work{};

	static constexpr nano::block_type block_type_v = nano::block_type::change;

	void serialize (nano::stream &) const;
	void deserialize (nano::stream &);
	nano::root root () const;
	bool operator== (raw_change_block const &) const = default;

	nano::block_hash previous_field () const;
	nano::account representative_field () const;
};

/*
 * raw_state_block
 */
struct raw_state_block
{
	nano::state_hashables hashables;
	nano::signature signature{};
	uint64_t work{};

	static constexpr nano::block_type block_type_v = nano::block_type::state;

	void serialize (nano::stream &) const;
	void deserialize (nano::stream &);
	nano::root root () const;
	bool operator== (raw_state_block const &) const = default;

	nano::account account_field () const;
	nano::amount balance_field () const;
	nano::link link_field () const;
	nano::block_hash previous_field () const;
	nano::account representative_field () const;
};

/*
 * raw_block_variant
 */
using raw_block_variant = std::variant<raw_send_block, raw_receive_block, raw_open_block, raw_change_block, raw_state_block>;

/*
 * raw_block
 */
class raw_block
{
	raw_block_variant data_m;
	nano::block_hash cached_hash_m{ 0 };

public:
	raw_block () = default;
	explicit raw_block (raw_block_variant data);
	explicit raw_block (raw_send_block);
	explicit raw_block (raw_receive_block);
	explicit raw_block (raw_open_block);
	explicit raw_block (raw_change_block);
	explicit raw_block (raw_state_block);

	// Core accessors
	nano::block_hash const & hash () const;
	nano::block_type type () const;
	nano::signature const & block_signature () const;
	nano::root root () const;
	uint64_t block_work () const;
	nano::qualified_root qualified_root () const;
	nano::block_hash previous () const;

	// Field accessors
	std::optional<nano::account> account_field () const;
	std::optional<nano::amount> balance_field () const;
	std::optional<nano::account> destination_field () const;
	std::optional<nano::link> link_field () const;
	std::optional<nano::block_hash> previous_field () const;
	std::optional<nano::account> representative_field () const;
	std::optional<nano::block_hash> source_field () const;

	// Serialization
	void serialize (nano::stream &) const;

	// Type-specific accessors
	std::optional<raw_send_block> as_send () const;
	std::optional<raw_receive_block> as_receive () const;
	std::optional<raw_open_block> as_open () const;
	std::optional<raw_change_block> as_change () const;
	std::optional<raw_state_block> as_state () const;

	// Variant access
	raw_block_variant const & variant () const;

	// Comparison
	bool operator== (raw_block const &) const = default;

	// Deserialize helper that catches exceptions and returns nullopt on failure
	template <typename T>
	static std::optional<T> try_deserialize (nano::stream & stream)
	{
		try
		{
			T block;
			block.deserialize (stream);
			return block;
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

private:
	nano::block_hash generate_hash () const;
};

// Conversion bridges
nano::raw_block to_raw (nano::block const & legacy);
std::shared_ptr<nano::block> to_legacy (nano::raw_block const & raw);

// Serialization with type prefix
nano::raw_block deserialize_raw_block (nano::stream &);
void serialize_raw_block (nano::stream &, nano::raw_block const &);
}
