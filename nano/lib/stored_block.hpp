#pragma once

#include <nano/lib/block_sideband.hpp>
#include <nano/lib/blocks_raw.hpp>

#include <boost/property_tree/ptree_fwd.hpp>

#include <optional>

namespace nano
{
/**
 * A value-type block that combines raw_block (transmitted data) with block_sideband (ledger metadata).
 * Sideband is always present. This is the type returned by ledger accessors for blocks stored in the ledger.
 */
class stored_block
{
	nano::raw_block raw_m;
	nano::block_sideband sideband_m;

public:
	stored_block () = default;
	stored_block (nano::raw_block raw, nano::block_sideband sideband);

	// Core accessors (delegated to raw_block)
	nano::block_hash const & hash () const;
	nano::block_type type () const;
	nano::work_version work_version () const;
	nano::signature const & block_signature () const;
	nano::root root () const;
	uint64_t block_work () const;
	uint64_t work () const;
	nano::qualified_root qualified_root () const;
	nano::block_hash previous () const;

	// Field accessors (delegated to raw_block, return optional)
	std::optional<nano::account> account_field () const;
	std::optional<nano::amount> balance_field () const;
	std::optional<nano::account> destination_field () const;
	std::optional<nano::link> link_field () const;
	std::optional<nano::block_hash> previous_field () const;
	std::optional<nano::account> representative_field () const;
	std::optional<nano::block_hash> source_field () const;

	// Canonical accessors (resolve via sideband when field not in block type)
	nano::account account () const noexcept;
	nano::amount balance () const noexcept;
	nano::account destination () const noexcept;
	nano::block_hash source () const noexcept;

	// Sideband access
	nano::block_sideband const & sideband () const;

	// Sideband-derived
	bool is_send () const noexcept;
	bool is_receive () const noexcept;
	bool is_change () const noexcept;
	bool is_epoch () const noexcept;
	nano::epoch epoch () const noexcept;
	std::array<nano::block_hash, 2> dependencies () const noexcept;

	// Raw access
	nano::raw_block const & raw () const;

	// Serialization
	void serialize_json (std::string &) const;
	void serialize_json (boost::property_tree::ptree &) const;
	std::string to_json () const;

	// Comparison
	bool operator== (stored_block const &) const;
};
}
