#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stored_block.hpp>
#include <nano/node/fwd.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <future>
#include <memory>
#include <thread>

namespace mi = boost::multi_index;

namespace nano::scheduler
{
class manual final
{
public:
	explicit manual (nano::node &);
	~manual ();

	void start ();
	void stop ();

	std::future<std::shared_ptr<nano::election>> push (nano::stored_block const & block);

	bool contains (nano::block_hash const &) const;

	nano::container_info container_info () const;

private:
	bool predicate () const;
	void notify ();
	void run ();

private: // Dependencies
	nano::node & node;

private:
	struct entry
	{
		nano::stored_block block;
		mutable std::promise<std::shared_ptr<nano::election>> promise;

		nano::block_hash hash () const
		{
			return block.hash ();
		}
	};

	// clang-format off
	class tag_sequenced {};
	class tag_hash {};

	using ordered_queue = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::const_mem_fun<entry, nano::block_hash, &entry::hash>>
	>>;
	// clang-format on

	ordered_queue queue;

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
};
}
