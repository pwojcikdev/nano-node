#include <nano/crypto/blake2/blake2.h>
#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/blocks_raw.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stream.hpp>

#include <boost/endian/conversion.hpp>

/*
 * raw_send_block
 */

void nano::raw_send_block::serialize (nano::stream & stream) const
{
	write (stream, hashables.previous.bytes);
	write (stream, hashables.destination.bytes);
	write (stream, hashables.balance.bytes);
	write (stream, signature.bytes);
	write (stream, work);
}

void nano::raw_send_block::deserialize (nano::stream & stream)
{
	read (stream, hashables.previous.bytes);
	read (stream, hashables.destination.bytes);
	read (stream, hashables.balance.bytes);
	read (stream, signature.bytes);
	read (stream, work);
}

nano::block_hash nano::raw_send_block::hash () const
{
	nano::block_hash result;
	blake2b_state hash_l;
	auto status = blake2b_init (&hash_l, sizeof (result.bytes));
	debug_assert (status == 0);
	hashables.hash (hash_l);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

nano::block_type nano::raw_send_block::type () const
{
	return block_type_v;
}

nano::work_version nano::raw_send_block::work_version () const
{
	return nano::work_version::work_1;
}

nano::root nano::raw_send_block::root () const
{
	return hashables.previous;
}

nano::amount nano::raw_send_block::balance_field () const
{
	return hashables.balance;
}

nano::account nano::raw_send_block::destination_field () const
{
	return hashables.destination;
}

nano::block_hash nano::raw_send_block::previous_field () const
{
	return hashables.previous;
}

uint64_t nano::raw_send_block::work_field () const
{
	return work;
}

/*
 * raw_receive_block
 */

void nano::raw_receive_block::serialize (nano::stream & stream) const
{
	write (stream, hashables.previous.bytes);
	write (stream, hashables.source.bytes);
	write (stream, signature.bytes);
	write (stream, work);
}

void nano::raw_receive_block::deserialize (nano::stream & stream)
{
	read (stream, hashables.previous.bytes);
	read (stream, hashables.source.bytes);
	read (stream, signature.bytes);
	read (stream, work);
}

nano::block_hash nano::raw_receive_block::hash () const
{
	nano::block_hash result;
	blake2b_state hash_l;
	auto status = blake2b_init (&hash_l, sizeof (result.bytes));
	debug_assert (status == 0);
	hashables.hash (hash_l);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

nano::block_type nano::raw_receive_block::type () const
{
	return block_type_v;
}

nano::work_version nano::raw_receive_block::work_version () const
{
	return nano::work_version::work_1;
}

nano::root nano::raw_receive_block::root () const
{
	return hashables.previous;
}

nano::block_hash nano::raw_receive_block::previous_field () const
{
	return hashables.previous;
}

nano::block_hash nano::raw_receive_block::source_field () const
{
	return hashables.source;
}

uint64_t nano::raw_receive_block::work_field () const
{
	return work;
}

/*
 * raw_open_block
 */

void nano::raw_open_block::serialize (nano::stream & stream) const
{
	write (stream, hashables.source);
	write (stream, hashables.representative);
	write (stream, hashables.account);
	write (stream, signature);
	write (stream, work);
}

void nano::raw_open_block::deserialize (nano::stream & stream)
{
	read (stream, hashables.source);
	read (stream, hashables.representative);
	read (stream, hashables.account);
	read (stream, signature);
	read (stream, work);
}

nano::block_hash nano::raw_open_block::hash () const
{
	nano::block_hash result;
	blake2b_state hash_l;
	auto status = blake2b_init (&hash_l, sizeof (result.bytes));
	debug_assert (status == 0);
	hashables.hash (hash_l);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

nano::block_type nano::raw_open_block::type () const
{
	return block_type_v;
}

nano::work_version nano::raw_open_block::work_version () const
{
	return nano::work_version::work_1;
}

nano::root nano::raw_open_block::root () const
{
	return hashables.account;
}

nano::account nano::raw_open_block::account_field () const
{
	return hashables.account;
}

nano::account nano::raw_open_block::representative_field () const
{
	return hashables.representative;
}

nano::block_hash nano::raw_open_block::source_field () const
{
	return hashables.source;
}

uint64_t nano::raw_open_block::work_field () const
{
	return work;
}

/*
 * raw_change_block
 */

void nano::raw_change_block::serialize (nano::stream & stream) const
{
	write (stream, hashables.previous);
	write (stream, hashables.representative);
	write (stream, signature);
	write (stream, work);
}

void nano::raw_change_block::deserialize (nano::stream & stream)
{
	read (stream, hashables.previous);
	read (stream, hashables.representative);
	read (stream, signature);
	read (stream, work);
}

nano::block_hash nano::raw_change_block::hash () const
{
	nano::block_hash result;
	blake2b_state hash_l;
	auto status = blake2b_init (&hash_l, sizeof (result.bytes));
	debug_assert (status == 0);
	hashables.hash (hash_l);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

nano::block_type nano::raw_change_block::type () const
{
	return block_type_v;
}

nano::work_version nano::raw_change_block::work_version () const
{
	return nano::work_version::work_1;
}

nano::root nano::raw_change_block::root () const
{
	return hashables.previous;
}

nano::block_hash nano::raw_change_block::previous_field () const
{
	return hashables.previous;
}

nano::account nano::raw_change_block::representative_field () const
{
	return hashables.representative;
}

uint64_t nano::raw_change_block::work_field () const
{
	return work;
}

/*
 * raw_state_block
 */

void nano::raw_state_block::serialize (nano::stream & stream) const
{
	write (stream, hashables.account);
	write (stream, hashables.previous);
	write (stream, hashables.representative);
	write (stream, hashables.balance);
	write (stream, hashables.link);
	write (stream, signature);
	write (stream, boost::endian::native_to_big (work));
}

void nano::raw_state_block::deserialize (nano::stream & stream)
{
	read (stream, hashables.account);
	read (stream, hashables.previous);
	read (stream, hashables.representative);
	read (stream, hashables.balance);
	read (stream, hashables.link);
	read (stream, signature);
	read (stream, work);
	boost::endian::big_to_native_inplace (work);
}

nano::block_hash nano::raw_state_block::hash () const
{
	nano::block_hash result;
	blake2b_state hash_l;
	auto status = blake2b_init (&hash_l, sizeof (result.bytes));
	debug_assert (status == 0);
	nano::uint256_union preamble (static_cast<uint64_t> (nano::block_type::state));
	blake2b_update (&hash_l, preamble.bytes.data (), preamble.bytes.size ());
	hashables.hash (hash_l);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

nano::block_type nano::raw_state_block::type () const
{
	return block_type_v;
}

nano::work_version nano::raw_state_block::work_version () const
{
	return nano::work_version::work_1;
}

nano::root nano::raw_state_block::root () const
{
	if (!hashables.previous.is_zero ())
	{
		return hashables.previous;
	}
	else
	{
		return hashables.account;
	}
}

nano::account nano::raw_state_block::account_field () const
{
	return hashables.account;
}

nano::amount nano::raw_state_block::balance_field () const
{
	return hashables.balance;
}

nano::link nano::raw_state_block::link_field () const
{
	return hashables.link;
}

nano::block_hash nano::raw_state_block::previous_field () const
{
	return hashables.previous;
}

nano::account nano::raw_state_block::representative_field () const
{
	return hashables.representative;
}

uint64_t nano::raw_state_block::work_field () const
{
	return work;
}

/*
 * raw_block
 */

nano::raw_block::raw_block (raw_block_variant data) :
	data_m{ std::move (data) },
	cached_hash_m{ generate_hash () }
{
}

nano::raw_block::raw_block (raw_send_block block) :
	data_m{ std::move (block) },
	cached_hash_m{ generate_hash () }
{
}

nano::raw_block::raw_block (raw_receive_block block) :
	data_m{ std::move (block) },
	cached_hash_m{ generate_hash () }
{
}

nano::raw_block::raw_block (raw_open_block block) :
	data_m{ std::move (block) },
	cached_hash_m{ generate_hash () }
{
}

nano::raw_block::raw_block (raw_change_block block) :
	data_m{ std::move (block) },
	cached_hash_m{ generate_hash () }
{
}

nano::raw_block::raw_block (raw_state_block block) :
	data_m{ std::move (block) },
	cached_hash_m{ generate_hash () }
{
}

nano::block_hash const & nano::raw_block::hash () const
{
	return cached_hash_m;
}

nano::block_type nano::raw_block::type () const
{
	return std::visit ([] (auto const & block) {
		return std::decay_t<decltype (block)>::block_type_v;
	},
	data_m);
}

nano::work_version nano::raw_block::work_version () const
{
	return nano::work_version::work_1;
}

uint64_t nano::raw_block::work () const
{
	return block_work ();
}

nano::signature const & nano::raw_block::block_signature () const
{
	return std::visit ([] (auto const & block) -> nano::signature const & {
		return block.signature;
	},
	data_m);
}

nano::root nano::raw_block::root () const
{
	return std::visit ([] (auto const & block) {
		return block.root ();
	},
	data_m);
}

uint64_t nano::raw_block::block_work () const
{
	return std::visit ([] (auto const & block) {
		return block.work;
	},
	data_m);
}

uint64_t nano::raw_block::work_field () const
{
	return block_work ();
}

nano::qualified_root nano::raw_block::qualified_root () const
{
	return { root (), previous () };
}

nano::block_hash nano::raw_block::previous () const
{
	return previous_field ().value_or (0);
}

std::optional<nano::account> nano::raw_block::account_field () const
{
	return std::visit ([] (auto const & block) -> std::optional<nano::account> {
		if constexpr (requires { block.account_field (); })
		{
			return block.account_field ();
		}
		else
		{
			return std::nullopt;
		}
	},
	data_m);
}

std::optional<nano::amount> nano::raw_block::balance_field () const
{
	return std::visit ([] (auto const & block) -> std::optional<nano::amount> {
		if constexpr (requires { block.balance_field (); })
		{
			return block.balance_field ();
		}
		else
		{
			return std::nullopt;
		}
	},
	data_m);
}

std::optional<nano::account> nano::raw_block::destination_field () const
{
	return std::visit ([] (auto const & block) -> std::optional<nano::account> {
		if constexpr (requires { block.destination_field (); })
		{
			return block.destination_field ();
		}
		else
		{
			return std::nullopt;
		}
	},
	data_m);
}

std::optional<nano::link> nano::raw_block::link_field () const
{
	return std::visit ([] (auto const & block) -> std::optional<nano::link> {
		if constexpr (requires { block.link_field (); })
		{
			return block.link_field ();
		}
		else
		{
			return std::nullopt;
		}
	},
	data_m);
}

std::optional<nano::block_hash> nano::raw_block::previous_field () const
{
	return std::visit ([] (auto const & block) -> std::optional<nano::block_hash> {
		if constexpr (requires { block.previous_field (); })
		{
			return block.previous_field ();
		}
		else
		{
			return std::nullopt;
		}
	},
	data_m);
}

std::optional<nano::account> nano::raw_block::representative_field () const
{
	return std::visit ([] (auto const & block) -> std::optional<nano::account> {
		if constexpr (requires { block.representative_field (); })
		{
			return block.representative_field ();
		}
		else
		{
			return std::nullopt;
		}
	},
	data_m);
}

std::optional<nano::block_hash> nano::raw_block::source_field () const
{
	return std::visit ([] (auto const & block) -> std::optional<nano::block_hash> {
		if constexpr (requires { block.source_field (); })
		{
			return block.source_field ();
		}
		else
		{
			return std::nullopt;
		}
	},
	data_m);
}

void nano::raw_block::serialize (nano::stream & stream) const
{
	std::visit ([&stream] (auto const & block) {
		block.serialize (stream);
	},
	data_m);
}

nano::raw_block_variant const & nano::raw_block::variant () const
{
	return data_m;
}

std::optional<nano::raw_send_block> nano::raw_block::as_send () const
{
	if (auto const * block = std::get_if<raw_send_block> (&data_m))
	{
		return *block;
	}
	return std::nullopt;
}

std::optional<nano::raw_receive_block> nano::raw_block::as_receive () const
{
	if (auto const * block = std::get_if<raw_receive_block> (&data_m))
	{
		return *block;
	}
	return std::nullopt;
}

std::optional<nano::raw_open_block> nano::raw_block::as_open () const
{
	if (auto const * block = std::get_if<raw_open_block> (&data_m))
	{
		return *block;
	}
	return std::nullopt;
}

std::optional<nano::raw_change_block> nano::raw_block::as_change () const
{
	if (auto const * block = std::get_if<raw_change_block> (&data_m))
	{
		return *block;
	}
	return std::nullopt;
}

std::optional<nano::raw_state_block> nano::raw_block::as_state () const
{
	if (auto const * block = std::get_if<raw_state_block> (&data_m))
	{
		return *block;
	}
	return std::nullopt;
}

nano::block_hash nano::raw_block::generate_hash () const
{
	nano::block_hash result;
	blake2b_state hash_l;
	auto status = blake2b_init (&hash_l, sizeof (result.bytes));
	debug_assert (status == 0);
	std::visit ([&hash_l] (auto const & block) {
		using T = std::decay_t<decltype (block)>;
		if constexpr (std::is_same_v<T, nano::raw_state_block>)
		{
			nano::uint256_union preamble (static_cast<uint64_t> (nano::block_type::state));
			blake2b_update (&hash_l, preamble.bytes.data (), preamble.bytes.size ());
		}
		block.hashables.hash (hash_l);
	},
	data_m);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

/*
 * Conversion bridges
 */

nano::raw_block nano::to_raw (nano::block const & legacy)
{
	switch (legacy.type ())
	{
		case nano::block_type::send:
		{
			auto const & b = static_cast<nano::send_block const &> (legacy);
			return nano::raw_block{ raw_send_block{ b.hashables, b.signature, b.work } };
		}
		case nano::block_type::receive:
		{
			auto const & b = static_cast<nano::receive_block const &> (legacy);
			return nano::raw_block{ raw_receive_block{ b.hashables, b.signature, b.work } };
		}
		case nano::block_type::open:
		{
			auto const & b = static_cast<nano::open_block const &> (legacy);
			return nano::raw_block{ raw_open_block{ b.hashables, b.signature, b.work } };
		}
		case nano::block_type::change:
		{
			auto const & b = static_cast<nano::change_block const &> (legacy);
			return nano::raw_block{ raw_change_block{ b.hashables, b.signature, b.work } };
		}
		case nano::block_type::state:
		{
			auto const & b = static_cast<nano::state_block const &> (legacy);
			return nano::raw_block{ raw_state_block{ b.hashables, b.signature, b.work } };
		}
		default:
			release_assert (false, "invalid block type in to_raw");
	}
}

std::shared_ptr<nano::block> nano::to_legacy (nano::raw_block const & raw)
{
	return std::visit ([] (auto const & block) -> std::shared_ptr<nano::block> {
		using T = std::decay_t<decltype (block)>;
		if constexpr (std::is_same_v<T, nano::raw_send_block>)
		{
			auto legacy = std::make_shared<nano::send_block> ();
			legacy->hashables = block.hashables;
			legacy->signature = block.signature;
			legacy->work = block.work;
			return legacy;
		}
		else if constexpr (std::is_same_v<T, nano::raw_receive_block>)
		{
			auto legacy = std::make_shared<nano::receive_block> ();
			legacy->hashables = block.hashables;
			legacy->signature = block.signature;
			legacy->work = block.work;
			return legacy;
		}
		else if constexpr (std::is_same_v<T, nano::raw_open_block>)
		{
			auto legacy = std::make_shared<nano::open_block> ();
			legacy->hashables = block.hashables;
			legacy->signature = block.signature;
			legacy->work = block.work;
			return legacy;
		}
		else if constexpr (std::is_same_v<T, nano::raw_change_block>)
		{
			auto legacy = std::make_shared<nano::change_block> ();
			legacy->hashables = block.hashables;
			legacy->signature = block.signature;
			legacy->work = block.work;
			return legacy;
		}
		else if constexpr (std::is_same_v<T, nano::raw_state_block>)
		{
			auto legacy = std::make_shared<nano::state_block> ();
			legacy->hashables = block.hashables;
			legacy->signature = block.signature;
			legacy->work = block.work;
			return legacy;
		}
	},
	raw.variant ());
}

nano::raw_block nano::deserialize_raw_block (nano::stream & stream)
{
	nano::block_type type;
	nano::read (stream, type);

	auto make = [&stream] (auto tag) -> nano::raw_block {
		using T = decltype (tag);
		auto result = nano::raw_block::try_deserialize<T> (stream);
		if (!result)
		{
			throw std::runtime_error ("Failed to deserialize block");
		}
		return nano::raw_block{ std::move (*result) };
	};

	switch (type)
	{
		case nano::block_type::send:
			return make (raw_send_block{});
		case nano::block_type::receive:
			return make (raw_receive_block{});
		case nano::block_type::open:
			return make (raw_open_block{});
		case nano::block_type::change:
			return make (raw_change_block{});
		case nano::block_type::state:
			return make (raw_state_block{});
		default:
			throw std::runtime_error ("Unknown block type");
	}
}

nano::raw_block nano::deserialize_raw_block (nano::stream & stream, nano::block_type type)
{
	auto make = [&stream] (auto tag) -> nano::raw_block {
		using T = decltype (tag);
		auto result = nano::raw_block::try_deserialize<T> (stream);
		if (!result)
		{
			throw std::runtime_error ("Failed to deserialize block");
		}
		return nano::raw_block{ std::move (*result) };
	};

	switch (type)
	{
		case nano::block_type::send:
			return make (raw_send_block{});
		case nano::block_type::receive:
			return make (raw_receive_block{});
		case nano::block_type::open:
			return make (raw_open_block{});
		case nano::block_type::change:
			return make (raw_change_block{});
		case nano::block_type::state:
			return make (raw_state_block{});
		default:
			throw std::runtime_error ("Unknown block type");
	}
}

void nano::serialize_raw_block (nano::stream & stream, nano::raw_block const & block)
{
	nano::write (stream, block.type ());
	block.serialize (stream);
}
