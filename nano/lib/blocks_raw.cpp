#include <nano/crypto/blake2/blake2.h>
#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks_raw.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stream.hpp>

#include <boost/endian/conversion.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <sstream>

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

void nano::raw_block::set_work (uint64_t work)
{
	std::visit ([work] (auto & b) {
		b.work = work;
	},
	data_m);
}

void nano::raw_block::set_signature (nano::signature const & signature)
{
	std::visit ([&signature] (auto & b) {
		b.signature = signature;
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

void nano::raw_block::serialize_json (std::string & string_a) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree);
	string_a = ostream.str ();
}

void nano::raw_block::serialize_json (boost::property_tree::ptree & tree_a) const
{
	std::visit ([&tree_a] (auto const & block) {
		using T = std::decay_t<decltype (block)>;
		if constexpr (std::is_same_v<T, nano::raw_send_block>)
		{
			tree_a.put ("type", "send");
			tree_a.put ("previous", block.hashables.previous.to_string ());
			tree_a.put ("destination", block.hashables.destination.to_account ());
			tree_a.put ("balance", block.hashables.balance.to_string ());
			tree_a.put ("work", nano::to_string_hex (block.work));
			tree_a.put ("signature", block.signature.to_string ());
		}
		else if constexpr (std::is_same_v<T, nano::raw_receive_block>)
		{
			tree_a.put ("type", "receive");
			tree_a.put ("previous", block.hashables.previous.to_string ());
			tree_a.put ("source", block.hashables.source.to_string ());
			tree_a.put ("work", nano::to_string_hex (block.work));
			tree_a.put ("signature", block.signature.to_string ());
		}
		else if constexpr (std::is_same_v<T, nano::raw_open_block>)
		{
			tree_a.put ("type", "open");
			tree_a.put ("source", block.hashables.source.to_string ());
			tree_a.put ("representative", block.hashables.representative.to_account ());
			tree_a.put ("account", block.hashables.account.to_account ());
			tree_a.put ("work", nano::to_string_hex (block.work));
			tree_a.put ("signature", block.signature.to_string ());
		}
		else if constexpr (std::is_same_v<T, nano::raw_change_block>)
		{
			tree_a.put ("type", "change");
			tree_a.put ("previous", block.hashables.previous.to_string ());
			tree_a.put ("representative", block.hashables.representative.to_account ());
			tree_a.put ("work", nano::to_string_hex (block.work));
			tree_a.put ("signature", block.signature.to_string ());
		}
		else if constexpr (std::is_same_v<T, nano::raw_state_block>)
		{
			tree_a.put ("type", "state");
			tree_a.put ("account", block.hashables.account.to_account ());
			tree_a.put ("previous", block.hashables.previous.to_string ());
			tree_a.put ("representative", block.hashables.representative.to_account ());
			tree_a.put ("balance", block.hashables.balance.to_string_dec ());
			tree_a.put ("link", block.hashables.link.to_string ());
			tree_a.put ("link_as_account", block.hashables.link.to_account ());
			tree_a.put ("signature", block.signature.to_string ());
			tree_a.put ("work", nano::to_string_hex (block.work));
		}
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

nano::raw_block nano::deserialize_raw_block_json (boost::property_tree::ptree const & tree)
{
	auto type_str = tree.get<std::string> ("type");
	auto signature_str = tree.get<std::string> ("signature");
	auto work_str = tree.get<std::string> ("work");

	nano::signature signature;
	if (signature.decode_hex (signature_str))
	{
		throw std::runtime_error ("Invalid signature");
	}
	uint64_t work;
	if (nano::from_string_hex (work_str, work))
	{
		throw std::runtime_error ("Invalid work");
	}

	if (type_str == "send")
	{
		nano::raw_send_block block;
		if (block.hashables.previous.decode_hex (tree.get<std::string> ("previous")))
			throw std::runtime_error ("Invalid previous");
		if (block.hashables.destination.decode_account (tree.get<std::string> ("destination")))
			throw std::runtime_error ("Invalid destination");
		if (block.hashables.balance.decode_hex (tree.get<std::string> ("balance")))
			throw std::runtime_error ("Invalid balance");
		block.signature = signature;
		block.work = work;
		return nano::raw_block{ std::move (block) };
	}
	else if (type_str == "receive")
	{
		nano::raw_receive_block block;
		if (block.hashables.previous.decode_hex (tree.get<std::string> ("previous")))
			throw std::runtime_error ("Invalid previous");
		if (block.hashables.source.decode_hex (tree.get<std::string> ("source")))
			throw std::runtime_error ("Invalid source");
		block.signature = signature;
		block.work = work;
		return nano::raw_block{ std::move (block) };
	}
	else if (type_str == "open")
	{
		nano::raw_open_block block;
		if (block.hashables.source.decode_hex (tree.get<std::string> ("source")))
			throw std::runtime_error ("Invalid source");
		if (block.hashables.representative.decode_account (tree.get<std::string> ("representative")))
			throw std::runtime_error ("Invalid representative");
		if (block.hashables.account.decode_account (tree.get<std::string> ("account")))
			throw std::runtime_error ("Invalid account");
		block.signature = signature;
		block.work = work;
		return nano::raw_block{ std::move (block) };
	}
	else if (type_str == "change")
	{
		nano::raw_change_block block;
		if (block.hashables.previous.decode_hex (tree.get<std::string> ("previous")))
			throw std::runtime_error ("Invalid previous");
		if (block.hashables.representative.decode_account (tree.get<std::string> ("representative")))
			throw std::runtime_error ("Invalid representative");
		block.signature = signature;
		block.work = work;
		return nano::raw_block{ std::move (block) };
	}
	else if (type_str == "state")
	{
		nano::raw_state_block block;
		if (block.hashables.account.decode_account (tree.get<std::string> ("account")))
			throw std::runtime_error ("Invalid account");
		if (block.hashables.previous.decode_hex (tree.get<std::string> ("previous")))
			throw std::runtime_error ("Invalid previous");
		if (block.hashables.representative.decode_account (tree.get<std::string> ("representative")))
			throw std::runtime_error ("Invalid representative");
		if (block.hashables.balance.decode_dec (tree.get<std::string> ("balance")))
			throw std::runtime_error ("Invalid balance");
		auto link_str = tree.get<std::string> ("link");
		if (block.hashables.link.decode_account (link_str) && block.hashables.link.decode_hex (link_str))
			throw std::runtime_error ("Invalid link");
		block.signature = signature;
		block.work = work;
		return nano::raw_block{ std::move (block) };
	}
	else
	{
		throw std::runtime_error ("Unknown block type: " + type_str);
	}
}

std::size_t nano::block_size (nano::block_type type)
{
	switch (type)
	{
		case nano::block_type::send:
			return nano::raw_send_block::size;
		case nano::block_type::receive:
			return nano::raw_receive_block::size;
		case nano::block_type::open:
			return nano::raw_open_block::size;
		case nano::block_type::change:
			return nano::raw_change_block::size;
		case nano::block_type::state:
			return nano::raw_state_block::size;
		default:
			debug_assert (false);
			return 0;
	}
}
