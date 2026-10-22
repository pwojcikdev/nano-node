#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/interval.hpp>
#include <nano/lib/locks.hpp>
#include <nano/node/endpoint.hpp>
#include <nano/node/fwd.hpp>

#include <deque>
#include <list>
#include <string>
#include <thread>
#include <unordered_map>

namespace nano
{
/**
 * Reaches out to peers gossiped in keepalives, peers remembered in the peer history and the preconfigured peers.
 * One thread ticks every merge period, merging one gossiped and one remembered peer per tick and running the preconfigured round on its period or on a trigger.
 * Each reachout to an address resolves and connects as a task on the io context.
 */
class network_reachout final
{
public:
	network_reachout (nano::node_config const &, nano::node_flags const &, nano::network &, nano::transport::tcp_listener &, nano::peer_history &, boost::asio::io_context &, nano::stats &, nano::logger &);
	~network_reachout ();

	void start ();
	void stop ();

	// Resolves the address, then connects to each of its endpoints without a channel and sends a keepalive down the channels that exist
	void reachout (std::string const & address, uint16_t port);
	// Makes the next preconfigured round run at once instead of at its period
	void trigger ();

	nano::container_info container_info () const;

private:
	void run ();
	// One gossiped peer per call, refilling from a sampled keepalive when the queue runs out
	void reachout_live ();
	// One remembered peer per call, refilling from the peer history when the queue runs out
	void reachout_cached ();
	// Every preconfigured peer, on the default port
	void reachout_preconfigured ();
	// Drops the finished reachout tasks
	void purge ();

	asio::awaitable<void> reachout_impl (std::string address, uint16_t port);
	// Reports the outcome of a preconfigured peer's connect, each outcome at most once an hour per peer, and nothing for other addresses
	void log_outcome (std::string const & address, nano::tcp_endpoint const &, std::error_code);

private: // Dependencies
	nano::node_config const & config;
	nano::node_flags const & flags;
	nano::network & network;
	nano::transport::tcp_listener & tcp_listener;
	nano::peer_history & peer_history;
	nano::stats & stats;
	nano::logger & logger;

private:
	nano::async::strand strand;
	boost::asio::ip::tcp::resolver resolver;
	// One task per reachout in flight, resolving and connecting on the strand
	std::list<nano::async::task> tasks;

	// Which sources the loop reaches out to, from the flags and the config
	bool const live_enabled;
	bool const cached_enabled;
	bool const preconfigured_enabled;

	// Peers still to reach out to from the last sampled keepalive and the last peer history read, used by the loop thread only
	std::deque<nano::endpoint> live_peers;
	std::deque<nano::endpoint> cached_peers;

	// Spaces the preconfigured rounds a warm-up or a full period apart, reset by a trigger
	nano::interval preconfigured_interval;
	// Set by a trigger and cleared by the loop, under mutex
	bool triggered{ false };

	// One entry per preconfigured peer, filled at construction and touched only on the strand
	struct peer_log
	{
		nano::interval success;
		nano::interval failure;
	};
	std::unordered_map<std::string, peer_log> preconfigured_logs;

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
};
}
