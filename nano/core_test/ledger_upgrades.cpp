/**
 * Tests for ledger database upgrades using the new universal backend architecture.
 *
 * These tests verify that database upgrades work correctly through the public
 * ledger_store and backend APIs, without relying on backend-specific implementation details.
 */

#include <nano/lib/files.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/make_store.hpp>
#include <nano/secure/account_info.hpp>
#include <nano/secure/common.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger/rep_weight.hpp>
#include <nano/store/ledger/version.hpp>
#include <nano/store/lmdb/backend_lmdb.hpp>
#include <nano/store/rocksdb/backend_rocksdb.hpp>
#include <nano/store/store.hpp>
#include <nano/store/tables.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/test_common/common.hpp>
#include <nano/test_common/make_store.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace std::chrono_literals;

namespace
{
/*
 * Schema definitions matching historical ledger versions
 */
nano::store::column_schema const schema_v21{
	{ nano::tables::blocks, "blocks" },
	{ nano::tables::accounts, "accounts" },
	{ nano::tables::pending, "pending" },
	{ nano::tables::online_weight, "online_weight" },
	{ nano::tables::pruned, "pruned" },
	{ nano::tables::peers, "peers" },
	{ nano::tables::confirmation_height, "confirmation_height" },
	{ nano::tables::final_votes, "final_votes" },
	{ nano::tables::frontiers, "frontiers" },
	{ nano::tables::unchecked, "unchecked" },
	{ nano::tables::meta, "meta" }
};

nano::store::column_schema const schema_v22{
	{ nano::tables::blocks, "blocks" },
	{ nano::tables::accounts, "accounts" },
	{ nano::tables::pending, "pending" },
	{ nano::tables::online_weight, "online_weight" },
	{ nano::tables::pruned, "pruned" },
	{ nano::tables::peers, "peers" },
	{ nano::tables::confirmation_height, "confirmation_height" },
	{ nano::tables::final_votes, "final_votes" },
	{ nano::tables::frontiers, "frontiers" },
	{ nano::tables::meta, "meta" }
};

nano::store::column_schema const schema_v23{
	{ nano::tables::blocks, "blocks" },
	{ nano::tables::accounts, "accounts" },
	{ nano::tables::pending, "pending" },
	{ nano::tables::rep_weights, "rep_weights" },
	{ nano::tables::online_weight, "online_weight" },
	{ nano::tables::pruned, "pruned" },
	{ nano::tables::peers, "peers" },
	{ nano::tables::confirmation_height, "confirmation_height" },
	{ nano::tables::final_votes, "final_votes" },
	{ nano::tables::frontiers, "frontiers" },
	{ nano::tables::meta, "meta" }
};

/*
 * Helper function to create a backend at a specific path
 */
std::unique_ptr<nano::store::backend> create_backend (std::filesystem::path const & path)
{
	auto backend_type = nano::node_config::env_database_backend ().value_or (nano::database_backend::lmdb);
	switch (backend_type)
	{
		case nano::database_backend::lmdb:
		{
			nano::lmdb_config lmdb_config{};
			return std::make_unique<nano::store::lmdb::backend_lmdb> (path / "data.ldb", nano::test::default_logger (), lmdb_config);
		}
		case nano::database_backend::rocksdb:
		{
			nano::rocksdb_config rocksdb_config{};
			return std::make_unique<nano::store::rocksdb::backend_rocksdb> (path / "rocksdb", rocksdb_config);
		}
	}
	release_assert (false, "unknown database backend");
}

/*
 * Helper function to get database path for ledger_store based on backend type
 */
std::filesystem::path get_db_path (std::filesystem::path const & base_path)
{
	auto backend_type = nano::node_config::env_database_backend ().value_or (nano::database_backend::lmdb);
	switch (backend_type)
	{
		case nano::database_backend::lmdb:
			return base_path / "data.ldb";
		case nano::database_backend::rocksdb:
			return base_path / "rocksdb";
	}
	release_assert (false, "unknown database backend");
}

/*
 * Helper class to create a v21 legacy database with test data
 */
class legacy_database_v21
{
public:
	legacy_database_v21 (std::filesystem::path const & path_a) :
		path{ path_a },
		backend{ create_backend (path_a) }
	{
		backend->create (schema_v21, 21);
	}

	// Insert dummy data into unchecked table for testing upgrade that drops it
	void add_unchecked (uint64_t key_value, uint64_t data_value)
	{
		backend->open (schema_v21, nano::store::open_mode::read_write);
		{
			auto tx = backend->tx_begin_write ();
			nano::store::db_val key{ sizeof (key_value), &key_value };
			nano::store::db_val value{ sizeof (data_value), &data_value };
			auto status = backend->put (tx, nano::tables::unchecked, key, value);
			backend->release_assert_success (status);
		}
		backend->close ();
	}

	void add_account (nano::account const & account, nano::account_info_v22 const & info)
	{
		backend->open (schema_v21, nano::store::open_mode::read_write);
		{
			auto tx = backend->tx_begin_write ();
			auto status = backend->put (tx, nano::tables::accounts, account, info);
			backend->release_assert_success (status);
		}
		backend->close ();
	}

	std::filesystem::path path;
	std::unique_ptr<nano::store::backend> backend;
};

/*
 * Helper class to create a v22 legacy database with test data
 */
class legacy_database_v22
{
public:
	legacy_database_v22 (std::filesystem::path const & path_a) :
		path{ path_a },
		backend{ create_backend (path_a) }
	{
		backend->create (schema_v22, 22);
	}

	void add_account (nano::account const & account, nano::account_info_v22 const & info)
	{
		backend->open (schema_v22, nano::store::open_mode::read_write);
		{
			auto tx = backend->tx_begin_write ();
			auto status = backend->put (tx, nano::tables::accounts, account, info);
			backend->release_assert_success (status);
		}
		backend->close ();
	}

	std::filesystem::path path;
	std::unique_ptr<nano::store::backend> backend;
};

/*
 * Helper class to create a v23 legacy database with test data
 */
class legacy_database_v23
{
public:
	legacy_database_v23 (std::filesystem::path const & path_a) :
		path{ path_a },
		backend{ create_backend (path_a) }
	{
		backend->create (schema_v23, 23);
	}

	std::filesystem::path path;
	std::unique_ptr<nano::store::backend> backend;
};
}

/*
 * Test that opening a database with a version higher than supported fails
 */
TEST (ledger_upgrades, version_too_high)
{
	auto path = nano::unique_path ();
	{
		// Create database with a future version
		auto backend = nano::test::make_backend (path);
		backend->create (nano::store::ledger_store::schema_current, 999);
	}

	// Attempting to open through ledger_store should throw
	ASSERT_THROW (
	nano::store::ledger_store (
	nano::test::make_backend (path),
	nano::store::open_mode::read_write,
	nano::test::default_stats (),
	nano::test::default_logger ()),
	std::runtime_error);
}

/*
 * Test that opening a database with a version lower than minimum fails
 */
TEST (ledger_upgrades, version_too_low)
{
	auto path = nano::unique_path ();
	{
		// Create database with a version below minimum
		auto backend = nano::test::make_backend (path);
		backend->create (nano::store::ledger_store::schema_current, 7);
	}

	// Attempting to open through ledger_store should throw
	ASSERT_THROW (
	nano::store::ledger_store (
	nano::test::make_backend (path),
	nano::store::open_mode::read_write,
	nano::test::default_stats (),
	nano::test::default_logger ()),
	std::runtime_error);
}

/*
 * Test that read-only mode prevents upgrades
 */
TEST (ledger_upgrades, read_only_prevents_upgrade)
{
	auto path = nano::unique_path ();
	{
		// Create a v21 database
		legacy_database_v21 legacy_db{ path };
	}

	// Attempting to open in read-only mode should fail because upgrade is needed
	ASSERT_THROW (
	nano::store::ledger_store (
	create_backend (path),
	nano::store::open_mode::read_only,
	nano::test::default_stats (),
	nano::test::default_logger ()),
	std::runtime_error);
}

/*
 * Test v21 to v22 upgrade: removes unchecked table
 */
TEST (ledger_upgrades, upgrade_v21_to_v22)
{
	auto path = nano::unique_path ();

	// Create a v21 database with data in unchecked table
	{
		legacy_database_v21 legacy_db{ path };
		// The unchecked table exists in the v21 schema
		legacy_db.add_unchecked (1, 100);
		legacy_db.add_unchecked (2, 200);
	}

	// Create ledger_store with defer_open to manually control upgrade
	nano::store::ledger_store_params params;
	params.defer_open = true;

	nano::store::ledger_store store (
		create_backend (path),
		nano::store::open_mode::read_write,
		nano::test::default_stats (),
		nano::test::default_logger (),
		params);

	// Manually perform just the v21->v22 upgrade
	store.upgrade_v21_to_v22 ();

	// Verify version is now 22 and unchecked table no longer exists
	auto backend = create_backend (path);
	backend->open (schema_v22, nano::store::open_mode::read_only);
	auto tx = backend->tx_begin_read ();
	ASSERT_EQ (backend->get_version (tx), 22);
	ASSERT_FALSE (backend->table_exists ("unchecked"));
}

/*
 * Test v22 to v23 upgrade: populates rep_weights from account data
 */
TEST (ledger_upgrades, upgrade_v22_to_v23_rep_weights)
{
	auto path = nano::unique_path ();

	nano::account const rep_a{ 100 };
	nano::account const rep_b{ 200 };
	nano::account const account_1{ 1 };
	nano::account const account_2{ 2 };
	nano::account const account_3{ 3 };

	// Create a v22 database with test accounts
	{
		legacy_database_v22 legacy_db{ path };

		// Account 1: balance 1000, rep_a
		nano::account_info_v22 info1{};
		info1.representative = rep_a;
		info1.balance = 1000;
		legacy_db.add_account (account_1, info1);

		// Account 2: balance 500, rep_a (same rep as account 1)
		nano::account_info_v22 info2{};
		info2.representative = rep_a;
		info2.balance = 500;
		legacy_db.add_account (account_2, info2);

		// Account 3: balance 42, rep_b
		nano::account_info_v22 info3{};
		info3.representative = rep_b;
		info3.balance = 42;
		legacy_db.add_account (account_3, info3);
	}

	// Open through ledger_store which should trigger upgrade
	auto store = nano::store::ledger_store (
	create_backend (path),
	nano::store::open_mode::read_write,
	nano::test::default_stats (),
	nano::test::default_logger ());

	// Verify rep weights were correctly calculated
	auto tx = store.tx_begin_read ();
	ASSERT_EQ (store.version.get (tx), nano::store::ledger_store::version_current);

	// rep_a should have weight from account_1 + account_2 = 1000 + 500 = 1500
	ASSERT_EQ (store.rep_weight.get (tx, rep_a), 1500);

	// rep_b should have weight from account_3 = 42
	ASSERT_EQ (store.rep_weight.get (tx, rep_b), 42);
}

/*
 * Test v22 to v23 upgrade: zero balance accounts don't contribute to rep weight
 */
TEST (ledger_upgrades, upgrade_v22_to_v23_zero_balance)
{
	auto path = nano::unique_path ();

	nano::account const rep{ 100 };
	nano::account const account_with_balance{ 1 };
	nano::account const account_zero_balance{ 2 };

	// Create a v22 database with accounts (one with zero balance)
	{
		legacy_database_v22 legacy_db{ path };

		nano::account_info_v22 info1{};
		info1.representative = rep;
		info1.balance = 1000;
		legacy_db.add_account (account_with_balance, info1);

		nano::account_info_v22 info2{};
		info2.representative = rep;
		info2.balance = 0; // Zero balance
		legacy_db.add_account (account_zero_balance, info2);
	}

	// Open through ledger_store which should trigger upgrade
	auto store = nano::store::ledger_store (
	create_backend (path),
	nano::store::open_mode::read_write,
	nano::test::default_stats (),
	nano::test::default_logger ());

	// Verify rep weight only includes non-zero balance accounts
	auto tx = store.tx_begin_read ();
	ASSERT_EQ (store.rep_weight.get (tx, rep), 1000);
}

/*
 * Test v23 to v24 upgrade: removes frontiers table
 */
TEST (ledger_upgrades, upgrade_v23_to_v24)
{
	auto path = nano::unique_path ();

	// Create a v23 database
	{
		legacy_database_v23 legacy_db{ path };
		// The frontiers table exists in the v23 schema
	}

	// Open through ledger_store which should trigger upgrade
	auto store = nano::store::ledger_store (
	create_backend (path),
	nano::store::open_mode::read_write,
	nano::test::default_stats (),
	nano::test::default_logger ());

	// Verify we're at current version after upgrade
	auto tx = store.tx_begin_read ();
	ASSERT_EQ (store.version.get (tx), nano::store::ledger_store::version_current);
}

/*
 * Test full upgrade path from v21 to current version
 */
TEST (ledger_upgrades, full_upgrade_v21_to_current)
{
	auto path = nano::unique_path ();

	nano::account const rep{ 100 };
	nano::account const account{ 1 };

	// Create a v21 database with test data
	{
		legacy_database_v21 legacy_db{ path };

		nano::account_info_v22 info{};
		info.representative = rep;
		info.balance = 5000;
		legacy_db.add_account (account, info);
	}

	// Open through ledger_store which should trigger full upgrade chain
	auto store = nano::store::ledger_store (
	create_backend (path),
	nano::store::open_mode::read_write,
	nano::test::default_stats (),
	nano::test::default_logger ());

	// Verify final version
	auto tx = store.tx_begin_read ();
	ASSERT_EQ (store.version.get (tx), nano::store::ledger_store::version_current);

	// Verify account still exists
	nano::account_info account_info;
	ASSERT_FALSE (store.account.get (tx, account, account_info));
	ASSERT_EQ (account_info.balance, 5000);

	// Verify rep_weight was populated during v22->v23 upgrade
	ASSERT_EQ (store.rep_weight.get (tx, rep), 5000);
}

/*
 * Test that a current version database opens without upgrade
 */
TEST (ledger_upgrades, current_version_no_upgrade)
{
	auto path = nano::unique_path ();

	// Create a current version database
	{
		auto store = nano::store::ledger_store (
		create_backend (path),
		nano::store::open_mode::read_write,
		nano::test::default_stats (),
		nano::test::default_logger ());

		auto tx = store.tx_begin_write ();
		store.initialize (tx, nano::dev::constants);
	}

	// Open again - should not require any upgrade
	auto store = nano::store::ledger_store (
	create_backend (path),
	nano::store::open_mode::read_write,
	nano::test::default_stats (),
	nano::test::default_logger ());

	auto tx = store.tx_begin_read ();
	ASSERT_EQ (store.version.get (tx), nano::store::ledger_store::version_current);
}

/*
 * Test opening an initialized store in read-only mode
 */
TEST (ledger_upgrades, current_version_read_only)
{
	auto path = nano::unique_path ();

	// Create and initialize a current version database
	{
		auto store = nano::store::ledger_store (
		create_backend (path),
		nano::store::open_mode::read_write,
		nano::test::default_stats (),
		nano::test::default_logger ());

		auto tx = store.tx_begin_write ();
		store.initialize (tx, nano::dev::constants);
	}

	// Open in read-only mode - should succeed since no upgrade needed
	auto store = nano::store::ledger_store (
	create_backend (path),
	nano::store::open_mode::read_only,
	nano::test::default_stats (),
	nano::test::default_logger ());

	auto tx = store.tx_begin_read ();
	ASSERT_EQ (store.version.get (tx), nano::store::ledger_store::version_current);

	// Verify we can read genesis account
	nano::account_info info;
	ASSERT_FALSE (store.account.get (tx, nano::dev::genesis_key.pub, info));
}

/*
 * Test v22->v23 upgrade with many accounts to verify batch processing
 */
TEST (ledger_upgrades, upgrade_v22_to_v23_batch_processing)
{
	auto path = nano::unique_path ();

	// Create multiple reps with multiple accounts
	std::vector<nano::account> reps;
	std::map<nano::account, nano::uint128_t> expected_weights;

	for (int i = 0; i < 5; ++i)
	{
		reps.push_back (nano::account{ static_cast<uint64_t> (1000 + i) });
		expected_weights[reps.back ()] = 0;
	}

	// Create a v22 database with many accounts
	{
		legacy_database_v22 legacy_db{ path };

		for (int i = 0; i < 50; ++i)
		{
			nano::account account{ static_cast<uint64_t> (i + 1) };
			auto & rep = reps[i % reps.size ()];
			nano::uint128_t balance = (i + 1) * 100;

			nano::account_info_v22 info{};
			info.representative = rep;
			info.balance = balance;
			legacy_db.add_account (account, info);

			expected_weights[rep] += balance;
		}
	}

	// Open through ledger_store which should trigger upgrade
	auto store = nano::store::ledger_store (
	create_backend (path),
	nano::store::open_mode::read_write,
	nano::test::default_stats (),
	nano::test::default_logger ());

	// Verify rep weights were correctly calculated
	auto tx = store.tx_begin_read ();
	for (auto const & [rep, expected_weight] : expected_weights)
	{
		ASSERT_EQ (store.rep_weight.get (tx, rep), expected_weight)
		<< "Rep weight mismatch for rep " << rep.to_account ();
	}
}
