#pragma once

#include <nano/lib/node_capabilities.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/transport/traffic_type.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>

namespace nano::bootstrap
{
enum class acquire_status
{
	acquired, // A matching peer was reserved
	busy, // Matching peers exist but are all at capacity; the caller should wait for capacity
	exhausted, // Every matching peer was rejected by the exclusion predicate; the caller should conclude the work
	no_peers, // No connected peer satisfies the capability requirement; the caller should wait for peers
};

struct acquire_result
{
	std::shared_ptr<nano::transport::channel> channel;
	acquire_status status{ acquire_status::no_peers };
	nano::account node_id{ 0 }; // Cached identity of the acquired peer
};

/*
 * Pool of peers usable for bootstrap requests, tracked together with their in-flight request load.
 * A peer is acquired for each outgoing request, reserving one slot of its capacity, and released when the
 * request concludes. The periodic update () tracks newly connected channels and drops closed ones; the
 * periodic decay () shrinks loads that drift upwards when responses are lost.
 *
 * Peer identity (node id) and capabilities are cached per entry so selection scans avoid locking each channel.
 * Channels are held by shared_ptr and liveness is checked only in update (), so a closed channel may be
 * offered for up to one update interval;
 *
 * Not internally synchronized: callers must hold the bootstrap mutex.
 */
class peer_pool
{
public:
	static nano::transport::traffic_type constexpr default_traffic_type = nano::transport::traffic_type::bootstrap;

	explicit peer_pool (nano::bootstrap_config const &);

	// Reserves the least-loaded peer that satisfies the capability requirement and is not excluded.
	// The exclusion list lets a fanout round route each of its requests to a distinct peer.
	acquire_result acquire (nano::node_capabilities_flags required = {}, std::span<nano::account const> exclude = {}, nano::transport::traffic_type traffic = default_traffic_type);

	// Returns one reserved capacity slot when a response arrives
	void release (std::shared_ptr<nano::transport::channel> const &);

	// Returns true if any peer satisfies the capability requirement and is not excluded, ignoring capacity
	bool has_candidate (nano::node_capabilities_flags required = {}, std::span<nano::account const> exclude = {}) const;

	// Tracks newly connected channels and drops closed ones
	void update (std::deque<std::shared_ptr<nano::transport::channel>> const & channels);

	// Decays the load of unanswered requests
	void decay ();

	void reset ();

	std::size_t size () const;
	std::size_t available () const;

	nano::container_info container_info () const;

private: // Dependencies
	nano::bootstrap_config const & config;

private:
	struct entry
	{
		std::shared_ptr<nano::transport::channel> channel;

		// Cached so selection scans do not need to lock the channel
		nano::account node_id;
		nano::node_capabilities_flags capabilities;

		// Number of requests sent to the peer and not yet concluded
		uint64_t outstanding{ 0 };

		bool capable (nano::node_capabilities_flags required) const
		{
			return (capabilities & required) == required;
		}
	};

	std::unordered_map<nano::transport::channel *, entry> entries;
};
}
