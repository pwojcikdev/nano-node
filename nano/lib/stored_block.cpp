#include <nano/lib/stored_block.hpp>

nano::stored_block::stored_block (nano::raw_block raw, nano::block_sideband sideband) :
	raw_m{ std::move (raw) },
	sideband_m{ std::move (sideband) }
{
}

nano::block_hash const & nano::stored_block::hash () const
{
	return raw_m.hash ();
}

nano::block_type nano::stored_block::type () const
{
	return raw_m.type ();
}

nano::work_version nano::stored_block::work_version () const
{
	return raw_m.work_version ();
}

nano::signature const & nano::stored_block::block_signature () const
{
	return raw_m.block_signature ();
}

nano::root nano::stored_block::root () const
{
	return raw_m.root ();
}

uint64_t nano::stored_block::block_work () const
{
	return raw_m.block_work ();
}

uint64_t nano::stored_block::work () const
{
	return raw_m.work ();
}

nano::qualified_root nano::stored_block::qualified_root () const
{
	return raw_m.qualified_root ();
}

nano::block_hash nano::stored_block::previous () const
{
	return raw_m.previous ();
}

std::optional<nano::account> nano::stored_block::account_field () const
{
	return raw_m.account_field ();
}

std::optional<nano::amount> nano::stored_block::balance_field () const
{
	return raw_m.balance_field ();
}

std::optional<nano::account> nano::stored_block::destination_field () const
{
	return raw_m.destination_field ();
}

std::optional<nano::link> nano::stored_block::link_field () const
{
	return raw_m.link_field ();
}

std::optional<nano::block_hash> nano::stored_block::previous_field () const
{
	return raw_m.previous_field ();
}

std::optional<nano::account> nano::stored_block::representative_field () const
{
	return raw_m.representative_field ();
}

std::optional<nano::block_hash> nano::stored_block::source_field () const
{
	return raw_m.source_field ();
}

nano::account nano::stored_block::account () const noexcept
{
	switch (type ())
	{
		case block_type::open:
		case block_type::state:
			return account_field ().value ();
		case block_type::change:
		case block_type::send:
		case block_type::receive:
			return sideband_m.account;
		default:
			release_assert (false);
	}
}

nano::amount nano::stored_block::balance () const noexcept
{
	switch (type ())
	{
		case nano::block_type::open:
		case nano::block_type::receive:
		case nano::block_type::change:
			return sideband_m.balance;
		case nano::block_type::send:
		case nano::block_type::state:
			return balance_field ().value ();
		default:
			release_assert (false);
	}
}

nano::account nano::stored_block::destination () const noexcept
{
	switch (type ())
	{
		case nano::block_type::send:
			return destination_field ().value ();
		case nano::block_type::state:
			release_assert (sideband_m.details.is_send);
			return link_field ().value ().as_account ();
		default:
			release_assert (false);
	}
}

nano::block_hash nano::stored_block::source () const noexcept
{
	switch (type ())
	{
		case nano::block_type::open:
		case nano::block_type::receive:
			return source_field ().value ();
		case nano::block_type::state:
			release_assert (sideband_m.details.is_receive);
			return link_field ().value ().as_block_hash ();
		default:
			release_assert (false);
	}
}

nano::block_sideband const & nano::stored_block::sideband () const
{
	return sideband_m;
}

bool nano::stored_block::is_send () const noexcept
{
	switch (type ())
	{
		case nano::block_type::send:
			return true;
		case nano::block_type::state:
			return sideband_m.details.is_send;
		default:
			return false;
	}
}

bool nano::stored_block::is_receive () const noexcept
{
	switch (type ())
	{
		case nano::block_type::receive:
		case nano::block_type::open:
			return true;
		case nano::block_type::state:
			return sideband_m.details.is_receive;
		default:
			return false;
	}
}

bool nano::stored_block::is_change () const noexcept
{
	switch (type ())
	{
		case nano::block_type::change:
			return true;
		case nano::block_type::state:
			if (link_field ().value ().is_zero ())
			{
				return true;
			}
			return false;
		default:
			return false;
	}
}

bool nano::stored_block::is_epoch () const noexcept
{
	switch (type ())
	{
		case nano::block_type::state:
			return sideband_m.details.is_epoch;
		default:
			return false;
	}
}

nano::epoch nano::stored_block::epoch () const noexcept
{
	if (type () == nano::block_type::state)
	{
		return sideband_m.details.epoch;
	}
	return nano::epoch::epoch_0;
}

std::array<nano::block_hash, 2> nano::stored_block::dependencies () const noexcept
{
	std::array<nano::block_hash, 2> result{ 0, 0 };
	result[0] = previous ();
	if (is_receive ())
	{
		// For genesis block, the source is the genesis account public key (not a real block hash)
		if (type () == nano::block_type::open && source () == account ().as_union ())
		{
			// Genesis block has no source dependency
		}
		else
		{
			result[1] = source ();
		}
	}
	return result;
}

nano::raw_block const & nano::stored_block::raw () const
{
	return raw_m;
}

void nano::stored_block::serialize_json (std::string & string_a) const
{
	raw_m.serialize_json (string_a);
}

void nano::stored_block::serialize_json (boost::property_tree::ptree & tree) const
{
	raw_m.serialize_json (tree);
}

std::string nano::stored_block::to_json () const
{
	std::string result;
	serialize_json (result);
	return result;
}

bool nano::stored_block::operator== (stored_block const & other) const
{
	return hash () == other.hash ();
}
