#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/observer_set.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <thread>

namespace mi = boost::multi_index;

namespace nano
{
class unchecked_map
{
	static constexpr size_t max_triggered_entries{ 1000000 }; // 1M entries

public:
	unchecked_map (unsigned max_size, nano::stats &, bool disable_delete);
	~unchecked_map ();

	void start ();
	void stop ();

	void put (nano::hash_or_account const & dependency, nano::unchecked_info const & info);
	void trigger (nano::hash_or_account const & dependency);

	bool exists (nano::unchecked_key const & key) const;
	void del (nano::unchecked_key const & key);
	void clear ();

	void for_each (
	std::function<void (nano::unchecked_key const &, nano::unchecked_info const &)> action, std::function<bool ()> predicate = [] () { return true; });
	void for_each (
	nano::hash_or_account const & dependency, std::function<void (nano::unchecked_key const &, nano::unchecked_info const &)> action, std::function<bool ()> predicate = [] () { return true; });
	std::vector<nano::unchecked_info> get (nano::block_hash const &);

	size_t count () const; // Same as `entries_size ()`
	size_t entries_size () const;
	size_t triggered_size () const;

	nano::container_info container_info () const;

public: // Events
	nano::observer_set<nano::unchecked_info const &> satisfied;

private:
	void run ();
	void trigger_impl (nano::block_hash const & hash);

private: // Dependencies
	nano::stats & stats;

private:
	unsigned const max_size;
	bool const disable_delete;

	std::deque<nano::hash_or_account> triggered;

	bool stopped{ false };
	nano::condition_variable condition;
	mutable std::recursive_mutex mutex;
	std::thread thread;

private:
	struct entry
	{
		nano::unchecked_key key;
		nano::unchecked_info info;
	};

	// clang-format off
	class tag_sequenced {};
	class tag_root {};
	class tag_hash {};

	using ordered_unchecked = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::ordered_unique<mi::tag<tag_root>,
			mi::member<entry, nano::unchecked_key, &entry::key>>
	>>;
	// clang-format on

	ordered_unchecked entries;

private:
	// Cache for recently triggered hashes to handle race conditions
	struct triggered_entry
	{
		nano::block_hash hash;
	};

	// clang-format off
	using triggered_cache = boost::multi_index_container<triggered_entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<triggered_entry, nano::block_hash, &triggered_entry::hash>>
	>>;
	// clang-format on

	triggered_cache recently_triggered;
};
}
