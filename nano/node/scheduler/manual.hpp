#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>

#include <boost/optional.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>

namespace nano::scheduler
{
class manual final
{
public:
	explicit manual (nano::node &);
	~manual ();

	void start ();
	void stop ();

	void push (std::shared_ptr<nano::block> const &, boost::optional<nano::uint128_t> const & = boost::none);

	bool contains (nano::block_hash const &) const;

	nano::container_info container_info () const;

private:
	bool predicate () const;
	void notify ();
	void run ();

private: // Dependencies
	nano::node & node;

private:
	std::deque<std::tuple<std::shared_ptr<nano::block>, boost::optional<nano::uint128_t>, nano::election_behavior>> queue;

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
};
}
