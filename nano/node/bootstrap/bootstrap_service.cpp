#include <nano/lib/thread_runner.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/bootstrap/bootstrap_service.hpp>
#include <nano/node/bootstrap/service.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>

#include <boost/property_tree/ptree.hpp>

#include <random>

nano::bootstrap_service::bootstrap_service (nano::node_config const & config_a, nano::ledger & ledger_a, nano::ledger_notifications & ledger_notifications_a,
nano::block_processor & block_processor_a, nano::network & network_a, nano::stats & stats_a, nano::logger & logger_a) :
	logger{ logger_a },
	io_ctx{ std::make_shared<asio::io_context> () },
	service_impl{ std::make_unique<nano::bootstrap::service> (
	config_a, ledger_a, ledger_notifications_a, block_processor_a,
	[&network_a, min_version = config_a.network_params.network.bootstrap_protocol_version_min] () {
		return network_a.list (/* all */ 0, min_version);
	},
	stats_a, logger_a, *io_ctx, clock, /* rng seed */ std::random_device{}()) },
	service{ *service_impl }
{
}

nano::bootstrap_service::~bootstrap_service ()
{
	debug_assert (!runner, "must be stopped before destruction");
}

void nano::bootstrap_service::start ()
{
	debug_assert (!runner);
	runner = std::make_unique<nano::thread_runner> (io_ctx, logger, 1, nano::thread_role::name::bootstrap);
	service.start ();
}

void nano::bootstrap_service::stop ()
{
	service.stop ();
	if (runner)
	{
		runner->abort ();
		runner->join ();
		runner.reset ();
	}
}

void nano::bootstrap_service::process (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const & channel)
{
	service.process (message, channel);
}

void nano::bootstrap_service::reset ()
{
	service.reset ();
}

void nano::bootstrap_service::prioritize (nano::account const & account)
{
	service.prioritize (account);
}

std::size_t nano::bootstrap_service::priority_size () const
{
	return service.priority_size ();
}

std::size_t nano::bootstrap_service::blocked_size () const
{
	return service.blocked_size ();
}

bool nano::bootstrap_service::prioritized (nano::account const & account) const
{
	return service.prioritized (account);
}

bool nano::bootstrap_service::blocked (nano::account const & account) const
{
	return service.blocked (account);
}

boost::property_tree::ptree nano::bootstrap_service::info () const
{
	// Copy the data via the strand, then serialize outside it
	auto [blocking, priorities] = service.info ();

	boost::property_tree::ptree result;

	// Priorities
	{
		boost::property_tree::ptree entries;
		for (auto const & entry : priorities)
		{
			boost::property_tree::ptree entry_l;
			entry_l.put ("account", entry.account.to_account ());
			entry_l.put ("priority", entry.priority);
			entries.push_back (std::make_pair ("", entry_l));
		}
		result.add_child ("priorities", entries);
	}
	// Blocking
	{
		boost::property_tree::ptree entries;
		for (auto const & entry : blocking)
		{
			boost::property_tree::ptree entry_l;
			entry_l.put ("account", entry.account.to_account ());
			entry_l.put ("dependency", entry.dependency.to_string ());
			entry_l.put ("dependency_account", entry.dependency_account.to_account ());
			entries.push_back (std::make_pair ("", entry_l));
		}
		result.add_child ("blocking", entries);
	}

	return result;
}

auto nano::bootstrap_service::status () const -> status_result
{
	return {
		.priorities = service.priority_size (),
		.blocking = service.blocked_size (),
	};
}

nano::container_info nano::bootstrap_service::container_info () const
{
	return service.container_info ();
}
