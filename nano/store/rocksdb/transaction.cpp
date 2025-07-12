#include <nano/store/rocksdb/transaction_impl.hpp>
#include <nano/store/transaction.hpp>

#include <chrono>
#include <thread>

nano::store::rocksdb::read_transaction_impl::read_transaction_impl (::rocksdb::DB * db_a) :
	db (db_a)
{
	if (db_a)
	{
		options.snapshot = db_a->GetSnapshot ();
	}
}

nano::store::rocksdb::read_transaction_impl::~read_transaction_impl ()
{
	reset ();
}

void nano::store::rocksdb::read_transaction_impl::reset ()
{
	if (db)
	{
		db->ReleaseSnapshot (options.snapshot);
	}
}

void nano::store::rocksdb::read_transaction_impl::renew ()
{
	options.snapshot = db->GetSnapshot ();
}

void * nano::store::rocksdb::read_transaction_impl::get_handle () const
{
	return (void *)&options;
}

nano::store::rocksdb::write_transaction_impl::write_transaction_impl (::rocksdb::OptimisticTransactionDB * db_a) :
	db (db_a)
{
	::rocksdb::OptimisticTransactionOptions txn_options;
	txn_options.set_snapshot = true;
	txn = db->BeginTransaction (::rocksdb::WriteOptions (), txn_options);
}

nano::store::rocksdb::write_transaction_impl::~write_transaction_impl ()
{
	commit ();
	delete txn;
}

void nano::store::rocksdb::write_transaction_impl::commit ()
{
	if (active)
	{
		auto status = txn->Commit ();

		// If there are no available memtables try again a few more times with cooldown
		constexpr auto max_attempts = 10;
		int attempt_num = 0;
		while (status.IsTryAgain () && attempt_num < max_attempts)
		{
			std::this_thread::sleep_for (15ms); // Small cooldown before retry
			status = txn->Commit ();
			++attempt_num;
		}

		if (!status.ok ())
		{
			if (status.IsBusy () || status.IsTimedOut ())
			{
				// Optimistic transaction conflict - throw exception
				throw nano::store::transaction_conflict_error ("Transaction commit failed due to conflict: " + status.ToString ());
			}
			else
			{
				// Other errors (including IsTryAgain after retries) should still crash
				release_assert (false, "Unable to write to the RocksDB database", status.ToString ());
			}
		}

		active = false;
	}
}

void nano::store::rocksdb::write_transaction_impl::renew ()
{
	::rocksdb::OptimisticTransactionOptions txn_options;
	txn_options.set_snapshot = true;
	db->BeginTransaction (::rocksdb::WriteOptions (), txn_options, txn);
	active = true;
}

void * nano::store::rocksdb::write_transaction_impl::get_handle () const
{
	return txn;
}

bool nano::store::rocksdb::write_transaction_impl::contains (nano::tables table_a) const
{
	return true;
}

bool nano::store::rocksdb::write_transaction_impl::check_no_write_tx () const
{
	return true; // TODO: No longer needed
}
