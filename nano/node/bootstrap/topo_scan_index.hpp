#pragma once

#include <nano/lib/container_info.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
enum class topo_head_type
{
	spear,
	repair,
};

using topo_head_index = uint64_t;

class topo_scan_index
{
public:
	explicit topo_scan_index (nano::topo_scan_config const &, nano::stats &);

	void seed (nano::topo_key start);
	void reset ();

	bool indexing () const;

	std::optional<nano::topo_key> next (topo_head_index);

	std::deque<nano::block_hash> next_fetch (topo_head_index, size_t max_count);

	std::deque<std::shared_ptr<nano::block>> next_blocks (topo_head_index);

	void process (topo_head_index, nano::topo_key const & start, std::deque<nano::topo_key> const & entries);

	void block_received (topo_head_index, std::shared_ptr<nano::block> const & block);

	void mark_processed (nano::block_hash const &);
	void mark_gapped (nano::block_hash const &);
	void mark_redundant (nano::block_hash const &);
	void mark_terminal (nano::block_hash const &);

private:
	struct page
	{
		std::deque<nano::block_hash> hashes;
		std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> blocks;
		std::unordered_map<nano::block_hash, nano::block_status> results;

		size_t count{ 0 };
		size_t processed{ 0 };
		size_t gapped{ 0 };
		size_t redundant{ 0 };
		size_t terminal{ 0 };
	};

private:
	class head
	{
		topo_head_type const type;
		std::deque<page> pages;
	};

	std::vector<head> heads;

	head & head_at (topo_head_index);
	head const & head_at (topo_head_index) const;
};
}
