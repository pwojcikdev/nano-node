#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/tomlconfig.hpp>
#include <nano/lib/vote.hpp>
#include <nano/messages/messages.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/bootstrap/bootstrap_server.hpp>
#include <nano/node/bootstrap/bootstrap_service.hpp>
#include <nano/node/message_processor.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/telemetry.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/node/vote_replier.hpp>
#include <nano/node/wallet.hpp>

#include <algorithm>

nano::message_processor::message_processor (message_processor_config const & config_a, nano::node & node_a) :
	config{ config_a },
	node{ node_a },
	stats{ node.stats },
	logger{ node.logger }
{
	// No threads would mean no processing at all, so there is always one shard
	auto const shard_count = std::max<size_t> (config.threads, 1);
	for (size_t n = 0; n < shard_count; ++n)
	{
		auto & shard_l = shards.emplace_back ();

		shard_l.queue.max_size_query = [this] (auto const & origin) {
			return config.max_queue;
		};

		shard_l.queue.priority_query = [this] (auto const & origin) {
			return 1;
		};
	}
}

nano::message_processor::~message_processor ()
{
	debug_assert (std::none_of (shards.begin (), shards.end (), [] (auto const & shard_l) { return shard_l.thread.joinable (); }));
}

void nano::message_processor::start ()
{
	for (auto & shard_l : shards)
	{
		debug_assert (!shard_l.thread.joinable ());

		shard_l.thread = std::thread ([this, &shard_l] () {
			nano::thread_role::set (nano::thread_role::name::message_processing);
			try
			{
				run (shard_l);
			}
			catch (boost::system::error_code & ec)
			{
				node.logger.critical (nano::log::type::network, "Error: {}", ec.message ());
				release_assert (false);
			}
			catch (std::error_code & ec)
			{
				node.logger.critical (nano::log::type::network, "Error: {}", ec.message ());
				release_assert (false);
			}
			catch (std::runtime_error & err)
			{
				node.logger.critical (nano::log::type::network, "Error: {}", err.what ());
				release_assert (false);
			}
			catch (...)
			{
				node.logger.critical (nano::log::type::network, "Unknown error");
				release_assert (false);
			}
		});
	}
}

void nano::message_processor::stop ()
{
	for (auto & shard_l : shards)
	{
		{
			nano::lock_guard<nano::mutex> lock{ shard_l.mutex };
			stopped = true;
		}
		shard_l.condition.notify_all ();
	}

	for (auto & shard_l : shards)
	{
		if (shard_l.thread.joinable ())
		{
			shard_l.thread.join ();
		}
	}
}

auto nano::message_processor::shard_for (std::shared_ptr<nano::transport::channel> const & channel) -> shard &
{
	// The channel's address, mixed by the splitmix64 finalizer since allocations differ in too few bits to spread by themselves
	uint64_t x = reinterpret_cast<uintptr_t> (channel.get ());
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return shards[x % shards.size ()];
}

bool nano::message_processor::put (std::unique_ptr<nano::messages::message> message, std::shared_ptr<nano::transport::channel> const & channel)
{
	release_assert (message != nullptr);
	release_assert (channel != nullptr);

	auto const type = message->type ();
	auto & shard_l = shard_for (channel);

	bool added = false;
	{
		nano::lock_guard<nano::mutex> guard{ shard_l.mutex };
		added = shard_l.queue.push ({ std::move (message), channel }, { nano::no_value{}, channel });
	}
	if (added)
	{
		stats.inc (nano::stat::type::message_processor, nano::stat::detail::process);
		stats.inc (nano::stat::type::message_processor_type, to_stat_detail (type));

		shard_l.condition.notify_one ();
	}
	else
	{
		stats.inc (nano::stat::type::message_processor, nano::stat::detail::overfill);
		stats.inc (nano::stat::type::message_processor_overfill, to_stat_detail (type));
	}
	return added;
}

void nano::message_processor::run (shard & shard_l)
{
	nano::unique_lock<nano::mutex> lock{ shard_l.mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::message_processor, nano::stat::detail::loop);

		if (!shard_l.queue.empty ())
		{
			// Only log if component is under pressure
			if (shard_l.queue.size () > nano::queue_warning_threshold () && shard_l.log_interval.elapse (15s))
			{
				logger.info (nano::log::type::message_processor, "{} messages in processing queue", shard_l.queue.size ());
			}

			run_batch (shard_l, lock);
			debug_assert (!lock.owns_lock ());
			lock.lock ();
		}
		else
		{
			shard_l.condition.wait (lock, [&] {
				return stopped || !shard_l.queue.empty ();
			});
		}
	}
}

void nano::message_processor::run_batch (shard & shard_l, nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());
	debug_assert (!shard_l.mutex.try_lock ());
	debug_assert (!shard_l.queue.empty ());

	nano::timer<std::chrono::milliseconds> timer;
	timer.start ();

	size_t const max_batch_size = 1024 * 4;
	auto batch = shard_l.queue.next_batch (max_batch_size);

	lock.unlock ();

	for (auto const & [entry, origin] : batch)
	{
		auto const & [message, channel] = entry;
		release_assert (message != nullptr);
		process (*message, channel);
	}

	if (timer.since_start () > std::chrono::milliseconds (100))
	{
		logger.debug (nano::log::type::message_processor, "Processed {} messages in {} milliseconds (rate of {} messages per second)",
		batch.size (),
		timer.since_start ().count (),
		((batch.size () * 1000ULL) / timer.value ().count ()));
	}
}

namespace
{
class process_visitor : public nano::messages::message_visitor
{
public:
	process_visitor (nano::node & node_a, std::shared_ptr<nano::transport::channel> const & channel_a) :
		node{ node_a },
		channel{ channel_a }
	{
	}

	void keepalive (nano::messages::keepalive const & message) override
	{
		// Check for self reported peering port
		auto self_report = message.peers[0];
		if (self_report.address () == boost::asio::ip::address_v6{} && self_report.port () != 0)
		{
			// Remember this for future forwarding to other peers
			nano::endpoint peering_endpoint{ channel->get_remote_endpoint ().address (), self_report.port () };
			channel->set_peering_endpoint (peering_endpoint);
		}
		channel->set_last_keepalive (message);
	}

	void publish (nano::messages::publish const & message) override
	{
		// A peering-only node keeps no ledger and does no block processing
		if (node.flags.peering_only)
		{
			node.stats.inc (nano::stat::type::message_drop, nano::stat::detail::publish, nano::stat::dir::in);
			return;
		}
		// Put blocks that are being initially broadcasted in a separate queue, so that they won't have to compete with rebroadcasted blocks
		// Both queues have the same priority and size, so the potential for exploiting this is limited
		debug_assert (message.digest != 0 || channel->get_type () != nano::transport::transport_type::tcp); // Messages received from the network should have their digest set
		bool added = node.block_processor.add (message.block, message.is_originator () ? nano::block_source::live_originator : nano::block_source::live, channel);
		if (!added)
		{
			node.network.filter.clear (message.digest);
			node.stats.inc (nano::stat::type::message_drop, nano::stat::detail::publish, nano::stat::dir::in);
		}
	}

	void confirm_req (nano::messages::confirm_req const & message) override
	{
		// Don't load nodes with disabled voting
		if (node.is_voting ())
		{
			if (!message.roots_hashes.empty ())
			{
				node.vote_replier.request (message.roots_hashes, channel);
			}
		}
	}

	void confirm_ack (nano::messages::confirm_ack const & message) override
	{
		// A peering-only node keeps no ledger and does no vote processing
		if (node.flags.peering_only)
		{
			node.stats.inc (nano::stat::type::message_drop, nano::stat::detail::confirm_ack, nano::stat::dir::in);
			return;
		}
		// Ignore zero account votes
		if (message.vote->account.is_zero ())
		{
			node.stats.inc (nano::stat::type::message_drop, nano::stat::detail::confirm_ack_zero_account, nano::stat::dir::in);
			return;
		}

		debug_assert (message.digest != 0 || channel->get_type () != nano::transport::transport_type::tcp); // Messages received from the network should have their digest set
		bool added = node.vote_processor.vote (message.vote, channel, message.is_rebroadcasted () ? nano::vote_source::rebroadcast : nano::vote_source::live);
		if (!added)
		{
			node.network.filter.clear (message.digest);
			node.stats.inc (nano::stat::type::message_drop, nano::stat::detail::confirm_ack, nano::stat::dir::in);
		}
	}

	void bulk_pull (nano::messages::bulk_pull const &) override
	{
		debug_assert (false);
	}

	void bulk_pull_account (nano::messages::bulk_pull_account const &) override
	{
		debug_assert (false);
	}

	void bulk_push (nano::messages::bulk_push const &) override
	{
		debug_assert (false);
	}

	void frontier_req (nano::messages::frontier_req const &) override
	{
		debug_assert (false);
	}

	void node_id_handshake (nano::messages::node_id_handshake const & message) override
	{
		node.stats.inc (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in);
	}

	void telemetry_req (nano::messages::telemetry_req const & message) override
	{
		// Ignore telemetry requests as telemetry is being periodically broadcasted since V25+
	}

	void telemetry_ack (nano::messages::telemetry_ack const & message) override
	{
		node.telemetry.process (message, channel);
	}

	void asc_pull_req (nano::messages::asc_pull_req const & message) override
	{
		node.bootstrap_server.request (message, channel);
	}

	void asc_pull_ack (nano::messages::asc_pull_ack const & message) override
	{
		node.bootstrap.process (message, channel);
	}

private:
	nano::node & node;
	std::shared_ptr<nano::transport::channel> channel;
};
}

void nano::message_processor::process (nano::messages::message const & message, std::shared_ptr<nano::transport::channel> const & channel)
{
	release_assert (channel != nullptr);

	debug_assert (message.header.network == node.network_params.network.current_network);
	debug_assert (message.header.version_using >= node.network_params.network.protocol_version_min);

	stats.inc (nano::stat::type::message, to_stat_detail (message.type ()), nano::stat::dir::in);
	logger.trace (nano::log::type::message, to_log_detail (message.type ()), nano::log::arg{ "message", message });

	process_visitor visitor{ node, channel };
	message.visit (visitor);
}

nano::container_info nano::message_processor::container_info () const
{
	nano::container_info info;
	size_t index = 0;
	for (auto const & shard_l : shards)
	{
		nano::lock_guard<nano::mutex> guard{ shard_l.mutex };
		info.add ("shard_" + std::to_string (index++), shard_l.queue.container_info ());
	}
	return info;
}

/*
 * message_processor_config
 */

nano::error nano::message_processor_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("threads", threads, "Number of threads to use for message processing. Each peer is served by one of them, so its messages are processed in the order they arrived. \ntype:uint64");
	toml.put ("max_queue", max_queue, "Maximum number of messages per peer to queue for processing. \ntype:uint64");

	return toml.get_error ();
}

nano::error nano::message_processor_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("threads", threads);
	toml.get ("max_queue", max_queue);

	return toml.get_error ();
}
