#include <nano/lib/stream.hpp>
#include <nano/secure/vote.hpp>
#include <nano/store/rocksdb/rocksdb.hpp>
#include <nano/store/rocksdb/utility.hpp>
#include <nano/store/rocksdb/vote_storage.hpp>

nano::store::rocksdb::vote_storage::vote_storage (nano::store::rocksdb::component & store) :
	store{ store } {};

std::size_t nano::store::rocksdb::vote_storage::put (store::write_transaction const & transaction, std::shared_ptr<nano::vote> const & vote_a)
{
	std::size_t count = 0;

	// Store vote for each hash in the vote
	for (auto const & hash : vote_a->hashes)
	{
		nano::vote_storage_key key{ hash, vote_a->account };

		// Check if vote already exists
		nano::store::rocksdb::db_val existing_value;
		auto status = store.get (transaction, tables::vote_storage, key, existing_value);

		bool should_put = false;

		if (store.not_found (status))
		{
			// No existing vote, insert this one
			should_put = true;
		}
		else if (store.success (status))
		{
			// Existing vote found, check if we should replace it
			auto existing_vote = std::make_shared<nano::vote> ();
			nano::bufferstream stream (static_cast<uint8_t const *> (existing_value.data ()), existing_value.size ());
			auto error = existing_vote->deserialize (stream);

			if (!error)
			{
				// Replace non-final votes with final votes, or if new vote has higher timestamp
				bool existing_is_final = existing_vote->is_final ();
				bool new_is_final = vote_a->is_final ();

				if (new_is_final && !existing_is_final)
				{
					// Replace non-final with final
					should_put = true;
				}
				else if (new_is_final == existing_is_final && vote_a->timestamp () > existing_vote->timestamp ())
				{
					// Replace with newer timestamp (both final or both non-final)
					should_put = true;
				}
			}
			else
			{
				// Error deserializing existing vote, replace it
				should_put = true;
			}
		}

		if (should_put)
		{
			// Serialize the vote
			std::vector<uint8_t> bytes;
			{
				nano::vectorstream stream (bytes);
				vote_a->serialize (stream);
			}

			status = store.put (transaction, tables::vote_storage, key, nano::store::rocksdb::db_val (bytes.size (), bytes.data ()));
			store.release_assert_success (status);
			++count;
		}
	}

	return count;
}

std::deque<std::shared_ptr<nano::vote>> nano::store::rocksdb::vote_storage::get (store::transaction const & transaction, nano::block_hash const & hash_a)
{
	std::deque<std::shared_ptr<nano::vote>> result;

	// Create a key with the hash and zero account to start iteration
	nano::vote_storage_key start_key{ hash_a, nano::account{ 0 } };

	// Iterate through all votes for this hash
	for (auto it = begin (transaction, start_key), end_it = end (transaction); it != end_it; ++it)
	{
		auto const & [key, vote] = *it;

		// Stop if we've moved past this hash
		if (key.hash () != hash_a)
		{
			break;
		}

		result.push_back (vote);
	}

	return result;
}

size_t nano::store::rocksdb::vote_storage::count (store::transaction const & transaction_a) const
{
	return store.count (transaction_a, tables::vote_storage);
}

void nano::store::rocksdb::vote_storage::clear (store::write_transaction const & transaction_a)
{
	store.drop (transaction_a, nano::tables::vote_storage);
}

auto nano::store::rocksdb::vote_storage::begin (store::transaction const & transaction, nano::vote_storage_key const & key) const -> iterator
{
	return iterator{ store::iterator{ rocksdb::iterator::lower_bound (store.db.get (), rocksdb::tx (transaction), store.table_to_column_family (tables::vote_storage), to_slice (key)) } };
}

auto nano::store::rocksdb::vote_storage::begin (store::transaction const & transaction) const -> iterator
{
	return iterator{ store::iterator{ rocksdb::iterator::begin (store.db.get (), rocksdb::tx (transaction), store.table_to_column_family (tables::vote_storage)) } };
}

auto nano::store::rocksdb::vote_storage::end (store::transaction const & transaction) const -> iterator
{
	return iterator{ store::iterator{ rocksdb::iterator::end (store.db.get (), rocksdb::tx (transaction), store.table_to_column_family (tables::vote_storage)) } };
}
