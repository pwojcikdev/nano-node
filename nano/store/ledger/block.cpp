#include <nano/secure/parallel_traversal.hpp>
#include <nano/store/db_val_templ.hpp>
#include <nano/store/ledger/block.hpp>

namespace nano::store::ledger
{
block::block (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

void block::put (nano::store::write_transaction const & transaction, nano::block_hash const & hash, nano::block const & block)
{
	class block_predecessor_set : public nano::block_visitor
	{
	public:
		block_predecessor_set (nano::store::write_transaction const & transaction_a, nano::store::ledger::block & block_store_a) :
			transaction{ transaction_a },
			block_store{ block_store_a }
		{
		}

		virtual ~block_predecessor_set () = default;

		// TODO: This is such an ugly code, refactor it
		void fill_value (nano::block const & block_a)
		{
			auto const hash = block_a.hash ();
			nano::store::db_val value;
			block_store.block_raw_get (transaction, block_a.previous (), value);
			debug_assert (value.size () != 0);
			auto const type = block_store.block_type_from_raw (value.data ());
			std::vector<uint8_t> data{ static_cast<uint8_t *> (value.data ()), static_cast<uint8_t *> (value.data ()) + value.size () };
			std::copy (hash.bytes.begin (), hash.bytes.end (), data.begin () + block_store.block_successor_offset (value.size (), type));
			block_store.raw_put (transaction, data, block_a.previous ());
		}

		void send_block (nano::send_block const & block_a) override
		{
			fill_value (block_a);
		}

		void receive_block (nano::receive_block const & block_a) override
		{
			fill_value (block_a);
		}

		void open_block (nano::open_block const & block_a) override
		{
		}

		void change_block (nano::change_block const & block_a) override
		{
			fill_value (block_a);
		}

		void state_block (nano::state_block const & block_a) override
		{
			if (!block_a.previous ().is_zero ())
			{
				fill_value (block_a);
			}
		}

	private:
		nano::store::write_transaction const & transaction;
		nano::store::ledger::block & block_store;
	};

	debug_assert (block.sideband ().successor.is_zero () || exists (transaction, block.sideband ().successor));
	std::vector<uint8_t> vector;
	{
		// TODO: WTF why reimplement deserialization?
		nano::vectorstream stream{ vector };
		nano::serialize_block (stream, block);
		block.sideband ().serialize (stream, block.type ());
	}
	raw_put (transaction, vector, hash);
	block_predecessor_set predecessor{ transaction, *this };
	block.visit (predecessor);
	debug_assert (block.previous ().is_zero () || successor (transaction, block.previous ()) == hash);
}

void block::raw_put (nano::store::write_transaction const & transaction, std::vector<uint8_t> const & data, nano::block_hash const & hash)
{
	db_val value{ data.size (), (void *)data.data () };
	auto status = backend.put (transaction, tables::blocks, hash, value);
	backend.release_assert_success (status);
}

std::optional<nano::block_hash> block::successor (nano::store::transaction const & transaction, nano::block_hash const & hash) const
{
	db_val value;
	block_raw_get (transaction, hash, value);
	nano::block_hash result;
	if (value.size () != 0)
	{
		debug_assert (value.size () >= result.bytes.size ());
		auto type = block_type_from_raw (value.data ());
		nano::bufferstream stream{ reinterpret_cast<uint8_t const *> (value.data ()) + block_successor_offset (value.size (), type), result.bytes.size () };
		bool error = nano::try_read (stream, result.bytes);
		(void)error;
		debug_assert (!error);
	}
	else
	{
		result.clear ();
	}
	if (result.is_zero ())
	{
		return std::nullopt;
	}
	return result;
}

void block::successor_clear (nano::store::write_transaction const & transaction, nano::block_hash const & hash)
{
	db_val value;
	block_raw_get (transaction, hash, value);
	debug_assert (value.size () != 0);
	auto type = block_type_from_raw (value.data ());
	std::vector<uint8_t> data{ static_cast<uint8_t *> (value.data ()), static_cast<uint8_t *> (value.data ()) + value.size () };
	std::fill_n (data.begin () + block_successor_offset (value.size (), type), sizeof (nano::block_hash), uint8_t{ 0 });
	raw_put (transaction, data, hash);
}

std::shared_ptr<nano::block> block::get (nano::store::transaction const & transaction, nano::block_hash const & hash) const
{
	db_val value;
	block_raw_get (transaction, hash, value);
	std::shared_ptr<nano::block> result;
	if (value.size () != 0)
	{
		nano::bufferstream stream{ reinterpret_cast<uint8_t const *> (value.data ()), value.size () };
		nano::block_type type;
		bool error = try_read (stream, type);
		release_assert (!error);
		result = nano::deserialize_block (stream, type);
		release_assert (result != nullptr);
		nano::block_sideband sideband;
		error = sideband.deserialize (stream, type);
		release_assert (!error);
		result->sideband_set (sideband);
	}
	return result;
}

void block::del (nano::store::write_transaction const & transaction, nano::block_hash const & hash)
{
	auto status = backend.del (transaction, tables::blocks, hash);
	backend.release_assert_success (status);
}

bool block::exists (nano::store::transaction const & transaction, nano::block_hash const & hash) const
{
	return backend.exists (transaction, tables::blocks, hash);
}

uint64_t block::count (nano::store::transaction const & transaction) const
{
	return backend.count (transaction, tables::blocks);
}

auto block::begin (nano::store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::blocks) };
}

auto block::begin (nano::store::transaction const & transaction, nano::block_hash const & hash) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::blocks, hash) };
}

auto block::end (nano::store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::blocks) };
}

void block::for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const
{
	parallel_traversal<nano::uint256_t> (
	[&action, this] (nano::uint256_t const & start, nano::uint256_t const & end, bool const is_last) {
		auto transaction = this->backend.tx_begin_read ();
		action (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end (transaction));
	});
}

void block::block_raw_get (nano::store::transaction const & transaction, nano::block_hash const & hash, nano::store::db_val & value) const
{
	auto status = backend.get (transaction, tables::blocks, hash, value);
	release_assert (backend.success (status) || backend.not_found (status), backend.error_string (status));
}

size_t block::block_successor_offset (size_t entry_size, nano::block_type type) const
{
	return entry_size - nano::block_sideband::size (type);
}

nano::block_type block::block_type_from_raw (void const * data) const
{
	return static_cast<nano::block_type> ((reinterpret_cast<uint8_t const *> (data))[0]);
}

}