#pragma once

#include <nano/node/fwd.hpp>

#include <memory>
#include <string>

namespace nano::scheduler
{
class component final
{
public:
	explicit component (nano::node & node);
	~component ();

	void start ();
	void stop ();

	/// Does the block exist in any of the schedulers
	bool exists (nano::block_hash const & hash) const;

	nano::container_info container_info () const;

private:
	std::unique_ptr<nano::scheduler::hinted> hinted_impl;
	std::unique_ptr<nano::scheduler::manual> manual_impl;
	std::unique_ptr<nano::scheduler::optimistic> optimistic_impl;
	std::unique_ptr<nano::scheduler::priority> priority_impl;

public: // Schedulers
	nano::scheduler::hinted & hinted;
	nano::scheduler::manual & manual;
	nano::scheduler::optimistic & optimistic;
	nano::scheduler::priority & priority;
};
}
