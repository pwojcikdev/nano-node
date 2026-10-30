#pragma once

#include <nano/lib/errors.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/scheduler/priority_pool.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>

namespace mi = boost::multi_index;

namespace nano::scheduler
{
class priority_config;

/**
 * A class which holds an ordered set of blocks to be scheduled, ordered by their block arrival time
 */
class bucket final
{
public:
	nano::bucket_index const index;

public:
	bucket (nano::bucket_index, priority_config const &, nano::active_elections &, nano::stats &, nano::logger &);
	~bucket ();

	bool available (priority_entry top) const;
	bool activate (priority_entry top);
	bool cleanup ();

	size_t election_count () const;

private:
	bool activate_predicate (nano::priority_timestamp candidate) const;
	bool overfill_predicate () const;
	bool cancel_lowest_election ();

private: // Dependencies
	priority_config const & config;
	nano::active_elections & active;
	nano::stats & stats;
	nano::logger & logger;

private: // Elections
	struct election_entry
	{
		std::shared_ptr<nano::election> election;
		nano::priority_timestamp priority;
	};

	// clang-format off
	class tag_sequenced {};
	class tag_election {};
	class tag_priority {};
	// clang-format on

	// Keyed by the election itself: an erase can race a new election for the same root, a root key would drop the newcomer's entry
	// clang-format off
	using ordered_elections = boost::multi_index_container<election_entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_election>,
			mi::member<election_entry, std::shared_ptr<nano::election>, &election_entry::election>>,
		mi::ordered_non_unique<mi::tag<tag_priority>,
			mi::member<election_entry, nano::priority_timestamp, &election_entry::priority>, std::greater<>> // Descending
	>>;
	// clang-format on

	ordered_elections elections;

private:
	mutable nano::mutex mutex;
};
}
