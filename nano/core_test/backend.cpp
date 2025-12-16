#include <nano/lib/files.hpp>
#include <nano/lib/logging.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/db_val.hpp>
#include <nano/store/tables.hpp>
#include <nano/test_common/make_store.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <thread>

namespace
{
// Test schema with meta (required) and accounts table for general testing
nano::store::column_schema const test_schema{
	{ nano::tables::meta, "meta" },
	{ nano::tables::accounts, "accounts" },
	{ nano::tables::blocks, "blocks" },
};

// Helper to create a simple key from an integer
nano::uint256_union make_key (uint64_t value)
{
	nano::uint256_union key{};
	key.qwords[0] = value;
	return key;
}

// Helper to create a simple value from an integer
nano::uint256_union make_value (uint64_t value)
{
	nano::uint256_union val{};
	val.qwords[0] = value;
	return val;
}
}

/*
 * Basic CRUD operations
 */

TEST (backend, basic_put_get)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	auto write_tx = backend->tx_begin_write ();
	auto put_status = backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
	ASSERT_TRUE (backend->success (put_status));
	write_tx.commit ();

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto get_status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	ASSERT_TRUE (backend->success (get_status));
	ASSERT_EQ (result.size (), sizeof (nano::uint256_union));
	nano::uint256_union result_value;
	std::memcpy (result_value.bytes.data (), result.data (), result.size ());
	EXPECT_EQ (result_value, value);
}

TEST (backend, put_overwrite)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value1 = make_value (100);
	auto value2 = make_value (200);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value1 });
	}
	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value2 });
	}

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	nano::uint256_union result_value;
	std::memcpy (result_value.bytes.data (), result.data (), result.size ());
	EXPECT_EQ (result_value, value2);
}

TEST (backend, get_non_existent)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (999);

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	EXPECT_TRUE (backend->not_found (status));
}

TEST (backend, exists_after_put)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	auto write_tx = backend->tx_begin_write ();
	EXPECT_FALSE (backend->exists (write_tx, nano::tables::accounts, nano::store::db_val{ key }));
	backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
	EXPECT_TRUE (backend->exists (write_tx, nano::tables::accounts, nano::store::db_val{ key }));
}

TEST (backend, delete_existing)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
	}
	{
		auto write_tx = backend->tx_begin_write ();
		EXPECT_TRUE (backend->exists (write_tx, nano::tables::accounts, nano::store::db_val{ key }));
		auto status = backend->del (write_tx, nano::tables::accounts, nano::store::db_val{ key });
		EXPECT_TRUE (backend->success (status));
		EXPECT_FALSE (backend->exists (write_tx, nano::tables::accounts, nano::store::db_val{ key }));
	}
}

TEST (backend, delete_non_existent)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (999);

	auto write_tx = backend->tx_begin_write ();
	EXPECT_FALSE (backend->exists (write_tx, nano::tables::accounts, nano::store::db_val{ key }));
	auto status = backend->del (write_tx, nano::tables::accounts, nano::store::db_val{ key });
	// Backends should return success for delete of non-existent key
	EXPECT_TRUE (backend->success (status));
}

TEST (backend, exists_after_delete)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
	}
	{
		auto write_tx = backend->tx_begin_write ();
		backend->del (write_tx, nano::tables::accounts, nano::store::db_val{ key });
	}

	auto read_tx = backend->tx_begin_read ();
	EXPECT_FALSE (backend->exists (read_tx, nano::tables::accounts, nano::store::db_val{ key }));
}

TEST (backend, get_after_delete)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
	}
	{
		auto write_tx = backend->tx_begin_write ();
		backend->del (write_tx, nano::tables::accounts, nano::store::db_val{ key });
	}

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	EXPECT_TRUE (backend->not_found (status));
}

/*
 * Binary data edge cases
 */

TEST (backend, empty_value)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	// Empty value (zero bytes)
	nano::store::db_val empty_value{ std::span<uint8_t const>{} };

	auto write_tx = backend->tx_begin_write ();
	auto put_status = backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, empty_value);
	ASSERT_TRUE (backend->success (put_status));
	write_tx.commit ();

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto get_status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	ASSERT_TRUE (backend->success (get_status));
	EXPECT_EQ (result.size (), 0);
}

TEST (backend, binary_null_bytes)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	// Key with embedded null bytes
	std::vector<uint8_t> key_data = { 0x01, 0x00, 0x02, 0x00, 0x03 };
	std::vector<uint8_t> value_data = { 0x00, 0xFF, 0x00, 0xFF, 0x00 };

	nano::store::db_val key{ std::span<uint8_t const>{ key_data } };
	nano::store::db_val value{ std::span<uint8_t const>{ value_data } };

	auto write_tx = backend->tx_begin_write ();
	auto put_status = backend->put (write_tx, nano::tables::accounts, key, value);
	ASSERT_TRUE (backend->success (put_status));
	write_tx.commit ();

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto get_status = backend->get (read_tx, nano::tables::accounts, key, result);
	ASSERT_TRUE (backend->success (get_status));
	ASSERT_EQ (result.size (), value_data.size ());
	EXPECT_EQ (std::memcmp (result.data (), value_data.data (), value_data.size ()), 0);
}

TEST (backend, binary_all_bytes)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	// Value with all possible byte values (0x00-0xFF)
	std::vector<uint8_t> all_bytes (256);
	for (int i = 0; i < 256; ++i)
	{
		all_bytes[i] = static_cast<uint8_t> (i);
	}

	auto key = make_key (1);
	nano::store::db_val value{ std::span<uint8_t const>{ all_bytes } };

	auto write_tx = backend->tx_begin_write ();
	auto put_status = backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, value);
	ASSERT_TRUE (backend->success (put_status));
	write_tx.commit ();

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto get_status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	ASSERT_TRUE (backend->success (get_status));
	ASSERT_EQ (result.size (), all_bytes.size ());
	EXPECT_EQ (std::memcmp (result.data (), all_bytes.data (), all_bytes.size ()), 0);
}

TEST (backend, large_value)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	// Large value (64KB)
	std::vector<uint8_t> large_data (64 * 1024);
	for (size_t i = 0; i < large_data.size (); ++i)
	{
		large_data[i] = static_cast<uint8_t> (i % 256);
	}

	auto key = make_key (1);
	nano::store::db_val value{ std::span<uint8_t const>{ large_data } };

	auto write_tx = backend->tx_begin_write ();
	auto put_status = backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, value);
	ASSERT_TRUE (backend->success (put_status));
	write_tx.commit ();

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto get_status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	ASSERT_TRUE (backend->success (get_status));
	ASSERT_EQ (result.size (), large_data.size ());
	EXPECT_EQ (std::memcmp (result.data (), large_data.data (), large_data.size ()), 0);
}

// Keys should be ordered lexicographically
TEST (backend, probe_key)
{
	// Test that 32-byte probe keys work with longer actual keys (like 64-byte/512-bit)
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	// Insert 64-byte keys with different first bytes
	{
		auto write_tx = backend->tx_begin_write ();
		for (uint8_t first_byte : { 0x20, 0x60, 0xA0, 0xE0 })
		{
			std::array<uint8_t, 64> key{};
			key[0] = first_byte;
			key[63] = 0x99; // Some data at the end
			backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ std::span<uint8_t const>{ key } }, nano::store::db_val{ make_value (first_byte) });
		}
	}

	auto read_tx = backend->tx_begin_read ();

	// Probe with 32-byte key (shorter than actual 64-byte keys)
	{
		std::array<uint8_t, 32> probe{};
		probe[0] = 0x50; // Between 0x20 and 0x60

		auto it = backend->begin (read_tx, nano::tables::accounts, nano::store::db_val{ std::span<uint8_t const>{ probe } });
		ASSERT_FALSE (it.is_end ());
		auto [k, v] = *it;
		EXPECT_EQ (k.size (), 64);
		EXPECT_EQ (k[0], 0x60); // Should find the 64-byte key starting with 0x60
	}

	// Probe with 32-byte key at boundary
	{
		std::array<uint8_t, 32> probe{};
		probe[0] = 0xA0;

		auto it = backend->begin (read_tx, nano::tables::accounts, nano::store::db_val{ std::span<uint8_t const>{ probe } });
		ASSERT_FALSE (it.is_end ());
		auto [k, v] = *it;
		EXPECT_EQ (k[0], 0xA0);
	}
}

/*
 * Iterators
 */

TEST (backend, iterator_empty_table)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto read_tx = backend->tx_begin_read ();
	auto begin_it = backend->begin (read_tx, nano::tables::accounts);
	auto end_it = backend->end (read_tx, nano::tables::accounts);

	EXPECT_TRUE (begin_it.is_end ());
	EXPECT_EQ (begin_it, end_it);
}

TEST (backend, iterator_single_entry)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
	}

	auto read_tx = backend->tx_begin_read ();
	auto it = backend->begin (read_tx, nano::tables::accounts);
	auto end_it = backend->end (read_tx, nano::tables::accounts);

	ASSERT_FALSE (it.is_end ());
	ASSERT_NE (it, end_it);

	auto [k, v] = *it;
	EXPECT_EQ (k.size (), sizeof (nano::uint256_union));

	++it;
	EXPECT_TRUE (it.is_end ());
}

TEST (backend, iterator_forward)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	// Insert keys in non-sorted order to verify ordering
	std::vector<nano::uint256_union> keys;
	for (uint64_t i : { 3, 1, 2 })
	{
		keys.push_back (make_key (i));
	}

	{
		auto write_tx = backend->tx_begin_write ();
		for (auto const & key : keys)
		{
			backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ make_value (0) });
		}
	}

	// Verify lexicographic order (key1 < key2 < key3)
	auto read_tx = backend->tx_begin_read ();
	auto it = backend->begin (read_tx, nano::tables::accounts);
	auto end_it = backend->end (read_tx, nano::tables::accounts);

	std::vector<nano::uint256_union> found_keys;
	for (; it != end_it; ++it)
	{
		auto [k, v] = *it;
		nano::uint256_union found_key;
		std::memcpy (found_key.bytes.data (), k.data (), k.size ());
		found_keys.push_back (found_key);
	}

	ASSERT_EQ (found_keys.size (), 3);
	EXPECT_EQ (found_keys[0], make_key (1));
	EXPECT_EQ (found_keys[1], make_key (2));
	EXPECT_EQ (found_keys[2], make_key (3));
}

TEST (backend, iterator_decrement_from_end)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key1 = make_key (1);
	auto key2 = make_key (2);
	auto key3 = make_key (3);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key2 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key3 }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();
	auto end_it = backend->end (read_tx, nano::tables::accounts);

	// Circular behavior: decrement end() should give last key (key3)
	--end_it;
	ASSERT_FALSE (end_it.is_end ());
	auto [k, v] = *end_it;
	nano::uint256_union found_key;
	std::memcpy (found_key.bytes.data (), k.data (), k.size ());
	EXPECT_EQ (found_key, key3);
}

TEST (backend, iterator_increment_from_end)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key1 = make_key (1);
	auto key2 = make_key (2);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key2 }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();
	auto end_it = backend->end (read_tx, nano::tables::accounts);

	// Circular behavior: increment end() should give first key (key1)
	++end_it;
	ASSERT_FALSE (end_it.is_end ());
	auto [k, v] = *end_it;
	nano::uint256_union found_key;
	std::memcpy (found_key.bytes.data (), k.data (), k.size ());
	EXPECT_EQ (found_key, key1);
}

TEST (backend, iterator_wrap_forward)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();
	auto it = backend->begin (read_tx, nano::tables::accounts);

	// After incrementing past the last element, should reach end
	++it;
	EXPECT_TRUE (it.is_end ());
}

TEST (backend, iterator_wrap_backward)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();
	auto it = backend->begin (read_tx, nano::tables::accounts);

	// Decrement from first element should wrap to end
	--it;
	EXPECT_TRUE (it.is_end ());
}

TEST (backend, iterator_lower_bound_exact)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key1 = make_key (1);
	auto key2 = make_key (2);
	auto key3 = make_key (3);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key2 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key3 }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();
	auto it = backend->begin (read_tx, nano::tables::accounts, nano::store::db_val{ key2 });

	ASSERT_FALSE (it.is_end ());
	auto [k, v] = *it;
	nano::uint256_union found_key;
	std::memcpy (found_key.bytes.data (), k.data (), k.size ());
	EXPECT_EQ (found_key, key2);
}

TEST (backend, iterator_lower_bound_between)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key1 = make_key (1);
	auto key3 = make_key (3);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key3 }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();
	auto key2 = make_key (2); // Key between key1 and key3
	auto it = backend->begin (read_tx, nano::tables::accounts, nano::store::db_val{ key2 });

	// Should find key3 (next key >= key2)
	ASSERT_FALSE (it.is_end ());
	auto [k, v] = *it;
	nano::uint256_union found_key;
	std::memcpy (found_key.bytes.data (), k.data (), k.size ());
	EXPECT_EQ (found_key, key3);
}

TEST (backend, iterator_lower_bound_past_all)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key1 = make_key (1);
	auto key2 = make_key (2);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key2 }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();
	auto large_key = make_key (999); // Key greater than all existing keys
	auto it = backend->begin (read_tx, nano::tables::accounts, nano::store::db_val{ large_key });

	// Should return end() since no key >= large_key
	EXPECT_TRUE (it.is_end ());
}

TEST (backend, iterator_reverse_traversal)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key1 = make_key (1);
	auto key2 = make_key (2);
	auto key3 = make_key (3);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key2 }, nano::store::db_val{ make_value (0) });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key3 }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();
	auto end_it = backend->end (read_tx, nano::tables::accounts);

	// Traverse backwards from end
	std::vector<nano::uint256_union> found_keys;
	--end_it;
	while (!end_it.is_end ())
	{
		auto [k, v] = *end_it;
		nano::uint256_union found_key;
		std::memcpy (found_key.bytes.data (), k.data (), k.size ());
		found_keys.push_back (found_key);
		--end_it;
	}

	ASSERT_EQ (found_keys.size (), 3);
	EXPECT_EQ (found_keys[0], key3); // Last key first
	EXPECT_EQ (found_keys[1], key2);
	EXPECT_EQ (found_keys[2], key1); // First key last
}

TEST (backend, iterator_is_end)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ make_value (0) });
	}

	auto read_tx = backend->tx_begin_read ();

	auto begin_it = backend->begin (read_tx, nano::tables::accounts);
	EXPECT_FALSE (begin_it.is_end ());

	auto end_it = backend->end (read_tx, nano::tables::accounts);
	EXPECT_TRUE (end_it.is_end ());
}

/*
 * Transactions
 */

TEST (backend, tx_read_basic)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
		write_tx.commit ();
	}

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	EXPECT_TRUE (backend->success (status));
}

TEST (backend, tx_write_commit)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
		write_tx.commit ();
	}

	// Verify data persisted after explicit commit
	auto read_tx = backend->tx_begin_read ();
	EXPECT_TRUE (backend->exists (read_tx, nano::tables::accounts, nano::store::db_val{ key }));
}

TEST (backend, tx_write_auto_commit)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
		// No explicit commit - destructor should commit
	}

	// Verify data persisted after auto-commit
	auto read_tx = backend->tx_begin_read ();
	EXPECT_TRUE (backend->exists (read_tx, nano::tables::accounts, nano::store::db_val{ key }));
}

TEST (backend, tx_refresh)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value1 = make_value (100);
	auto value2 = make_value (200);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value1 });
		write_tx.commit ();
	}

	// Start read transaction
	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	nano::uint256_union result_value;
	std::memcpy (result_value.bytes.data (), result.data (), result.size ());
	EXPECT_EQ (result_value, value1);

	// Update value in separate write transaction
	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value2 });
		write_tx.commit ();
	}

	// After refresh, read_tx should see new value
	read_tx.refresh ();
	backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
	std::memcpy (result_value.bytes.data (), result.data (), result.size ());
	EXPECT_EQ (result_value, value2);
}

TEST (backend, tx_epoch_increments)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto read_tx = backend->tx_begin_read ();
	auto initial_epoch = read_tx.epoch ();

	read_tx.refresh ();
	auto new_epoch = read_tx.epoch ();

	EXPECT_GT (new_epoch, initial_epoch);
}

TEST (backend, tx_multiple_read)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
		write_tx.commit ();
	}

	// Multiple concurrent read transactions should work
	auto read_tx1 = backend->tx_begin_read ();
	auto read_tx2 = backend->tx_begin_read ();

	EXPECT_TRUE (backend->exists (read_tx1, nano::tables::accounts, nano::store::db_val{ key }));
	EXPECT_TRUE (backend->exists (read_tx2, nano::tables::accounts, nano::store::db_val{ key }));
}

TEST (backend, tx_read_sees_committed)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (1);
	auto value = make_value (42);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
		write_tx.commit ();
	}

	// Read transaction started after commit should see data
	auto read_tx = backend->tx_begin_read ();
	EXPECT_TRUE (backend->exists (read_tx, nano::tables::accounts, nano::store::db_val{ key }));
}

TEST (backend, tx_read_isolation)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key1 = make_key (1);
	auto key2 = make_key (2);
	auto key3 = make_key (3);
	auto value1 = make_value (100);
	auto value2 = make_value (200);

	// Initial write - add key1
	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ value1 });
	}

	// Start read transaction (snapshot) - should see key1=value1, no key2, no key3
	auto read_tx = backend->tx_begin_read ();

	// Concurrent writes: update key1, add key2, add key3
	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ value2 });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key2 }, nano::store::db_val{ value2 });
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key3 }, nano::store::db_val{ value2 });
	}

	// Test get() isolation - should see old value for key1
	{
		nano::store::db_val result;
		auto status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key1 }, result);
		ASSERT_TRUE (backend->success (status));
		nano::uint256_union result_value;
		std::memcpy (result_value.bytes.data (), result.data (), result.size ());
		EXPECT_EQ (result_value, value1);
	}

	// Test exists() isolation - should not see key2 that was added after snapshot
	{
		EXPECT_TRUE (backend->exists (read_tx, nano::tables::accounts, nano::store::db_val{ key1 }));
		EXPECT_FALSE (backend->exists (read_tx, nano::tables::accounts, nano::store::db_val{ key2 }));
	}

	// Test iterator isolation - should only see key1, not key2 or key3
	{
		auto it = backend->begin (read_tx, nano::tables::accounts);
		auto end = backend->end (read_tx, nano::tables::accounts);

		std::vector<nano::uint256_union> found_keys;
		for (; it != end; ++it)
		{
			auto [k, v] = *it;
			nano::uint256_union found_key;
			std::memcpy (found_key.bytes.data (), k.data (), k.size ());
			found_keys.push_back (found_key);
		}

		ASSERT_EQ (found_keys.size (), 1);
		EXPECT_EQ (found_keys[0], key1);
	}

	// Test iterator with lower_bound isolation
	{
		auto it = backend->begin (read_tx, nano::tables::accounts, nano::store::db_val{ key2 });
		// key2 doesn't exist in snapshot, and key3 also doesn't exist, so should be end
		EXPECT_TRUE (it.is_end ());
	}
}

TEST (backend, tx_write_read_isolation)
{
	// Skip test for LMDB as it does not support concurrent write transactions
	if (nano::default_database_backend () == nano::database_backend::lmdb)
	{
		GTEST_SKIP ();
	}

	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key1 = make_key (1);
	auto key2 = make_key (2);
	auto key3 = make_key (3);
	auto value1 = make_value (100);
	auto value2 = make_value (200);

	// Initial write - add key1
	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ value1 });
	}

	// Start write transaction - snapshot taken here
	auto write_tx = backend->tx_begin_write ();

	// Another write transaction commits changes AFTER our write_tx started
	{
		auto other_write_tx = backend->tx_begin_write ();
		backend->put (other_write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, nano::store::db_val{ value2 });
		backend->put (other_write_tx, nano::tables::accounts, nano::store::db_val{ key2 }, nano::store::db_val{ value2 });
		backend->put (other_write_tx, nano::tables::accounts, nano::store::db_val{ key3 }, nano::store::db_val{ value2 });
	}

	// Test get() isolation within write transaction - should see old value for key1
	{
		nano::store::db_val result;
		auto status = backend->get (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }, result);
		ASSERT_TRUE (backend->success (status));
		nano::uint256_union result_value;
		std::memcpy (result_value.bytes.data (), result.data (), result.size ());
		EXPECT_EQ (result_value, value1);
	}

	// Test exists() isolation within write transaction
	{
		EXPECT_TRUE (backend->exists (write_tx, nano::tables::accounts, nano::store::db_val{ key1 }));
		EXPECT_FALSE (backend->exists (write_tx, nano::tables::accounts, nano::store::db_val{ key2 }));
	}

	// Test iterator isolation within write transaction
	{
		auto it = backend->begin (write_tx, nano::tables::accounts);
		auto end = backend->end (write_tx, nano::tables::accounts);

		std::vector<nano::uint256_union> found_keys;
		for (; it != end; ++it)
		{
			auto [k, v] = *it;
			nano::uint256_union found_key;
			std::memcpy (found_key.bytes.data (), k.data (), k.size ());
			found_keys.push_back (found_key);
		}

		ASSERT_EQ (found_keys.size (), 1);
		EXPECT_EQ (found_keys[0], key1);
	}
}

/*
 * Table operations
 */

TEST (backend, count_empty)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto read_tx = backend->tx_begin_read ();
	EXPECT_EQ (backend->count (read_tx, nano::tables::accounts), 0);
}

TEST (backend, count_accuracy)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	{
		auto write_tx = backend->tx_begin_write ();
		for (uint64_t i = 0; i < 10; ++i)
		{
			backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ make_key (i) }, nano::store::db_val{ make_value (i) });
		}
	}

	auto read_tx = backend->tx_begin_read ();
	EXPECT_EQ (backend->count (read_tx, nano::tables::accounts), 10);

	// Delete some entries
	{
		auto write_tx = backend->tx_begin_write ();
		backend->del (write_tx, nano::tables::accounts, nano::store::db_val{ make_key (0) });
		backend->del (write_tx, nano::tables::accounts, nano::store::db_val{ make_key (1) });
	}

	auto read_tx2 = backend->tx_begin_read ();
	EXPECT_EQ (backend->count (read_tx2, nano::tables::accounts), 8);
}

TEST (backend, empty_true_false)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	{
		auto read_tx = backend->tx_begin_read ();
		EXPECT_TRUE (backend->empty (read_tx, nano::tables::accounts));
	}
	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ make_key (1) }, nano::store::db_val{ make_value (1) });
	}
	{
		auto read_tx = backend->tx_begin_read ();
		EXPECT_FALSE (backend->empty (read_tx, nano::tables::accounts));
	}
}

TEST (backend, empty_vs_count)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	// empty() should be reliable even when count() might return estimates
	{
		auto read_tx = backend->tx_begin_read ();
		EXPECT_TRUE (backend->empty (read_tx, nano::tables::accounts));
		// When empty, count should be 0 or close to 0
		auto count = backend->count (read_tx, nano::tables::accounts);
		EXPECT_EQ (count, 0);
	}

	// Add entries
	{
		auto write_tx = backend->tx_begin_write ();
		for (uint64_t i = 0; i < 100; ++i)
		{
			backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ make_key (i) }, nano::store::db_val{ make_value (i) });
		}
	}

	{
		auto read_tx = backend->tx_begin_read ();
		EXPECT_FALSE (backend->empty (read_tx, nano::tables::accounts));
	}
}

TEST (backend, clear_table)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	{
		auto write_tx = backend->tx_begin_write ();
		for (uint64_t i = 0; i < 10; ++i)
		{
			backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ make_key (i) }, nano::store::db_val{ make_value (i) });
		}
	}
	{
		auto write_tx = backend->tx_begin_write ();
		auto status = backend->clear (write_tx, nano::tables::accounts);
		EXPECT_TRUE (backend->success (status));
	}

	auto read_tx = backend->tx_begin_read ();
	EXPECT_TRUE (backend->empty (read_tx, nano::tables::accounts));
}

TEST (backend, clear_empty_table)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto write_tx = backend->tx_begin_write ();
	// Clear on empty table should succeed
	auto status = backend->clear (write_tx, nano::tables::accounts);
	EXPECT_TRUE (backend->success (status));
}

TEST (backend, table_exists)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto read_tx = backend->tx_begin_read ();
	EXPECT_TRUE (backend->table_exists (read_tx, "accounts"));
	EXPECT_TRUE (backend->table_exists (read_tx, "meta"));
	EXPECT_FALSE (backend->table_exists (read_tx, "nonexistent_table"));
}

TEST (backend, drop_table)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ make_key (1) }, nano::store::db_val{ make_value (1) });
	}

	{
		auto read_tx = backend->tx_begin_read ();
		EXPECT_TRUE (backend->table_exists (read_tx, "accounts"));
	}

	{
		auto write_tx = backend->tx_begin_write ();
		EXPECT_TRUE (backend->drop_table (write_tx, "accounts"));
	}

	{
		auto read_tx = backend->tx_begin_read ();
		EXPECT_FALSE (backend->table_exists (read_tx, "accounts"));
	}

	// Drop non-existent table should return false
	{
		auto write_tx = backend->tx_begin_write ();
		EXPECT_FALSE (backend->drop_table (write_tx, "nonexistent_table"));
	}
}

// Test dropping a table that exists in the database but not in the current schema
// This simulates database upgrades where old tables need to be removed
TEST (backend, drop_table_not_in_schema)
{
	auto path = nano::unique_path ();

	// Schema with an extra table (simulating old version)
	nano::store::column_schema const old_schema{
		{ nano::tables::meta, "meta" },
		{ nano::tables::accounts, "accounts" },
		{ nano::tables::frontiers, "frontiers" }, // Dropped in v24
	};

	// Create database with old schema
	auto backend = nano::test::make_backend (path);
	{
		backend->create (old_schema, 1);
		backend->open (old_schema, nano::store::open_mode::read_write);

		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::frontiers, nano::store::db_val{ make_key (1) }, nano::store::db_val{ make_value (1) });
	}
	backend->close ();

	// Reopen with new schema (without frontiers) and drop the old table
	backend->open (test_schema, nano::store::open_mode::read_write);
	{
		auto read_tx = backend->tx_begin_read ();
		EXPECT_TRUE (backend->table_exists (read_tx, "frontiers"));
	}
	{
		auto write_tx = backend->tx_begin_write ();
		EXPECT_TRUE (backend->drop_table (write_tx, "frontiers"));
	}
	{
		auto read_tx = backend->tx_begin_read ();
		EXPECT_FALSE (backend->table_exists (read_tx, "frontiers"));
	}
}

/*
 * Lifecycle
 */

TEST (backend, open_create_close)
{
	auto path = nano::unique_path ();
	{
		auto backend = nano::test::make_backend (path);
		backend->create (test_schema, 1);
		backend->open (test_schema, nano::store::open_mode::read_write);

		auto key = make_key (1);
		auto value = make_value (42);

		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
		write_tx.commit ();

		backend->close ();
	}
}

TEST (backend, reopen_persistence)
{
	auto path = nano::unique_path ();
	auto key = make_key (1);
	auto value = make_value (42);

	// Write data
	{
		auto backend = nano::test::make_backend (path);
		backend->create (test_schema, 1);
		backend->open (test_schema, nano::store::open_mode::read_write);

		auto write_tx = backend->tx_begin_write ();
		backend->put (write_tx, nano::tables::accounts, nano::store::db_val{ key }, nano::store::db_val{ value });
		write_tx.commit ();

		backend->close ();
	}

	// Reopen and verify
	{
		auto backend = nano::test::make_backend (path);
		backend->open (test_schema, nano::store::open_mode::read_write);

		auto read_tx = backend->tx_begin_read ();
		EXPECT_TRUE (backend->exists (read_tx, nano::tables::accounts, nano::store::db_val{ key }));

		nano::store::db_val result;
		backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);
		nano::uint256_union result_value;
		std::memcpy (result_value.bytes.data (), result.data (), result.size ());
		EXPECT_EQ (result_value, value);
	}
}

TEST (backend, get_database_path)
{
	auto path = nano::unique_path ();
	auto backend = nano::test::make_backend (path);
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto db_path = backend->get_database_path ();
	// Path should contain the unique path we specified
	EXPECT_FALSE (db_path.empty ());
	EXPECT_NE (db_path.find (path.string ()), std::string::npos);
}

/*
 * Versioning
 */

TEST (backend, set_get_version)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	{
		auto write_tx = backend->tx_begin_write ();
		backend->set_version (write_tx, 42);
		write_tx.commit ();
	}

	auto read_tx = backend->tx_begin_read ();
	EXPECT_EQ (backend->get_version (read_tx), 42);
}

/*
 * Status codes
 */

TEST (backend, success_not_found)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (999);

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);

	EXPECT_FALSE (backend->success (status));
	EXPECT_TRUE (backend->not_found (status));
}

TEST (backend, error_string)
{
	auto backend = nano::test::make_backend ();
	backend->create (test_schema, 1);
	backend->open (test_schema, nano::store::open_mode::read_write);

	auto key = make_key (999);

	auto read_tx = backend->tx_begin_read ();
	nano::store::db_val result;
	auto status = backend->get (read_tx, nano::tables::accounts, nano::store::db_val{ key }, result);

	auto error_msg = backend->error_string (status);
	EXPECT_FALSE (error_msg.empty ());
}
