#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/bootstrap/common.hpp>

#include <chrono>
#include <span>
#include <vector>

namespace nano::bootstrap
{
class round
{
public:
	virtual ~round () = default;

	std::span<nano::account const> used () const;
	std::span<nano::account const> exclude () const;

	size_t launched () const;
	size_t inflight () const;
	std::chrono::steady_clock::time_point last_launch () const;

	virtual void conclude (id_t tag_id, nano::bootstrap::conclusion);

	bool owns (id_t tag_id) const;
	bool erase (id_t tag_id);
	void reserve (nano::account const & node_id, id_t tag_id, std::chrono::steady_clock::time_point now);

protected:
	round () = default;

private:
	std::vector<nano::account> used_m;
	std::vector<id_t> tag_ids_m;
	size_t launched_m{ 0 };
	std::chrono::steady_clock::time_point last_launch_m{};
};
}
