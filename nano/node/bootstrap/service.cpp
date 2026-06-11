#include <nano/lib/blocks.hpp>
#include <nano/lib/config.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/bootstrap/database_strategy.hpp>
#include <nano/node/bootstrap/dependency_strategy.hpp>
#include <nano/node/bootstrap/frontier_strategy.hpp>
#include <nano/node/bootstrap/priority_strategy.hpp>
#include <nano/node/bootstrap/service.hpp>
#include <nano/node/bootstrap/verify.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

#include <cmath>

using namespace std::chrono_literals;

namespace nano::bootstrap
{
service::service (nano::node_config const & node_config_a, nano::ledger & ledger_a, nano::ledger_notifications & ledger_notifications_a,
nano::block_processor & block_processor_a, nano::network & network_a, nano::stats & stats_a, nano::logger & logger_a,
asio::io_context & io_ctx_a, nano::async::clock & clock_a, unsigned rng_seed) :
	config{ *node_config_a.bootstrap },
	network_constants{ node_config_a.network_params.network },
	ledger{ ledger_a },
	ledger_notifications{ ledger_notifications_a },
	block_processor{ block_processor_a },
	network{ network_a },
	stats{ stats_a },
	logger{ logger_a },
	strand{ io_ctx_a.get_executor () },
	clock{ clock_a },
	workers{ 1, nano::thread_role::name::bootstrap_worker },
	accounts{ config.account_sets, stats },
	database_scan{ ledger },
	throttle{ compute_throttle_size () },
	peers{ config },
	accounts_changed{ strand },
	peers_changed{ strand },
	processor_drained{ strand },
	request_budget{ strand, config.max_requests },
	limiter{ config.rate_limit },
	database_limiter{ config.database_rate_limit },
	frontiers_limiter{ config.frontier_rate_limit },
	frontier{ std::make_unique<frontier_strategy> (*this) },
	priority{ std::make_unique<priority_strategy> (*this) },
	database{ std::make_unique<database_strategy> (*this) },
	dependency{ std::make_unique<dependency_strategy> (*this) },
	restart_condition{ strand },
	rng{ rng_seed }
{
	// Inspect all processed blocks: resolve the ledger-dependent values on the notification thread,
	// where the read transaction is cheap, and apply the decisions against live state on the strand
	ledger_notifications.blocks_processed.add ([this] (auto const & batch) {
		std::deque<inspect_command> commands;
		{
			auto transaction = this->ledger.tx_begin_read ();
			for (auto const & [result, block_context] : batch)
			{
				debug_assert (block_context.block != nullptr);
				if (auto command = resolve_inspect (this->ledger, transaction, result, *block_context.block, block_context.source))
				{
					commands.push_back (*command);
				}
			}
		}
		if (!commands.empty ())
		{
			asio::post (strand, [this, commands = std::move (commands)] () {
				for (auto const & command : commands)
				{
					apply_inspect (command);
				}
				accounts_changed.notify_all ();
			});
		}
		processor_drained.notify_all ();
	});

	// Unblock rolled back accounts as the dependency is no longer valid
	ledger_notifications.blocks_rolled_back.add ([this] (auto const & blocks, auto const & rollback_root) {
		std::deque<nano::account> rolled_back;
		for (auto const & block : blocks)
		{
			debug_assert (block != nullptr);
			rolled_back.push_back (block->account ());
		}
		asio::post (strand, [this, rolled_back = std::move (rolled_back)] () {
			for (auto const & account : rolled_back)
			{
				accounts.unblock (account);
			}
			accounts_changed.notify_all ();
		});
	});

	accounts.priority_set (node_config_a.network_params.ledger.genesis->account_field ().value ());
}

service::~service ()
{
	debug_assert (!supervisor || !supervisor->running ());
	debug_assert (!workers.alive ());
}

void service::start ()
{
	debug_assert (!supervisor);

	if (!config.enable)
	{
		logger.warn (nano::log::type::bootstrap, "Bootstrap is disabled, node will not be able to synchronize with the network");
		return;
	}

	workers.start ();

	supervisor.emplace (strand, [this] () -> asio::awaitable<void> {
		return run_supervisor ();
	});
}

void service::stop ()
{
	if (stopped.exchange (true))
	{
		return;
	}
	if (supervisor)
	{
		supervisor->cancel ();
		supervisor->join ();
	}
	workers.stop ();
}

void service::reset ()
{
	asio::post (strand, [this] () {
		restart_requested = true;
		restart_condition.notify_all ();
	});
}

asio::awaitable<void> service::run_supervisor ()
{
	debug_assert (strand.running_in_this_thread ());

	while (true)
	{
		nano::async::scope roots{ strand };

		roots.spawn (run_maintenance ());
		if (config.enable_priorities)
		{
			roots.spawn (priority->run ());
		}
		if (config.enable_database_scan)
		{
			roots.spawn (database->run ());
		}
		if (config.enable_dependency_walker)
		{
			roots.spawn (dependency->run ());
		}
		if (config.enable_frontier_scan)
		{
			roots.spawn (frontier->run ());
		}

		bool restarting = false;
		try
		{
			co_await nano::async::until (restart_condition, clock, [this] () {
				return restart_requested;
			});
			restarting = true;
		}
		catch (boost::system::system_error const & ex)
		{
			debug_assert (ex.code () == asio::error::operation_aborted); // Stopping
		}

		co_await asio::this_coro::reset_cancellation_state ();
		roots.cancel ();

		bool join_interrupted = false;
		try
		{
			co_await roots.join ();
		}
		catch (boost::system::system_error const & ex)
		{
			// A stop request arrived while restarting; the children are already cancelled, finish joining
			debug_assert (ex.code () == asio::error::operation_aborted);
			join_interrupted = true;
		}
		if (join_interrupted) // co_await is not allowed inside an exception handler
		{
			restarting = false;
			co_await asio::this_coro::reset_cancellation_state ();
			co_await roots.join ();
		}

		if (!restarting)
		{
			break;
		}

		restart_requested = false;
		reset_state ();
	}
}

void service::reset_state ()
{
	debug_assert (strand.running_in_this_thread ());

	stats.inc (nano::stat::type::bootstrap, nano::stat::detail::reset);
	logger.info (nano::log::type::bootstrap, "Resetting bootstrap state");

	accounts.reset ();
	database_scan.reset ();
	peers.reset ();
	throttle.reset ();
	// Frontier scan state lives in the strategy coroutines and resets when they are respawned
}

asio::awaitable<void> service::run_maintenance ()
{
	debug_assert (strand.running_in_this_thread ());

	while (true)
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_maintenance);

		// Snapshot peers off the strand: listing locks the network mutex and may block
		auto channels = co_await nano::async::offload (workers, [this] () {
			return network.list (/* all */ 0, network_constants.bootstrap_protocol_version_min);
		});
		peers.update (channels);
		peers.decay ();

		auto throttle_size = co_await nano::async::offload (workers, [this] () {
			return compute_throttle_size ();
		});
		throttle.resize (throttle_size);

		accounts.decay_blocking ();

		peers_changed.notify_all ();
		accounts_changed.notify_all ();

		co_await clock.sleep_for (nano::is_dev_run () ? 500ms : 5s);
	}
}

void service::process (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const & channel)
{
	// Bound the response ingress: a flood of unsolicited replies must not grow the queue without
	// limit. Today this backpressure was provided implicitly by processing inline under the mutex.
	if (pending_deliveries.load () >= config.max_requests)
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::response_overflow, nano::stat::dir::in);
		return;
	}
	pending_deliveries.fetch_add (1);

	asio::post (strand, [this, message, channel] () {
		pending_deliveries.fetch_sub (1);
		deliver (message, channel);
	});
}

void service::deliver (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const & channel)
{
	debug_assert (strand.running_in_this_thread ());

	// Only process messages that have a known in-flight request
	auto it = requests.find (message.id);
	if (it == requests.end ())
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::missing_tag);
		return;
	}

	// Verifies that response type corresponds to our query
	struct payload_verifier
	{
		query_type type;

		bool operator() (nano::messages::asc_pull_ack::blocks_payload const & response) const
		{
			return type == query_type::blocks_by_hash || type == query_type::blocks_by_account;
		}
		bool operator() (nano::messages::asc_pull_ack::account_info_payload const & response) const
		{
			return type == query_type::account_info_by_hash;
		}
		bool operator() (nano::messages::asc_pull_ack::frontiers_payload const & response) const
		{
			return type == query_type::frontiers;
		}
		bool operator() (nano::messages::empty_payload const & response) const
		{
			return false; // Should not happen
		}
	};

	bool valid = std::visit (payload_verifier{ it->second.type }, message.payload);
	if (!valid)
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::invalid_response_type);
		return;
	}

	stats.inc (nano::stat::type::bootstrap, nano::stat::detail::reply);
	stats.inc (nano::stat::type::bootstrap_reply, to_stat_detail (it->second.type));

	// Track bootstrap request response time
	auto const duration = std::chrono::duration_cast<std::chrono::milliseconds> (clock.now () - it->second.timestamp).count ();
	stats.sample (nano::stat::sample::bootstrap_tag_duration, duration, { 0, config.request_timeout.count () });

	auto slot = it->second.slot;
	requests.erase (it); // The request is consumed, a duplicate response counts as a missing tag

	slot->set (reply{ message, channel }); // Resumes the strategy coroutine awaiting this request
}

asio::awaitable<request_outcome> service::request (std::shared_ptr<nano::transport::channel> channel, query_descriptor query, query_source source, nano::async::semaphore::token budget)
{
	debug_assert (strand.running_in_this_thread ());

	id_t const id = generate_id ();
	auto const type = to_query_type (query);
	auto message = build_message (query, network_constants, id);

	auto slot = std::make_shared<nano::async::oneshot<reply>> (strand);
	requests.emplace (id, pending_request{ type, source, clock.now (), slot });

	// The correlation entry must be removed on every exit path, including cancellation unwind
	nano::scope_guard erase{ [this, id] () {
		requests.erase (id);
	} };

	// The wire-send confirmation arrives from a transport thread and is marshalled onto the strand
	auto sent = std::make_shared<nano::async::oneshot<bool>> (strand);
	bool queued = channel->send (message, nano::transport::traffic_type::bootstrap_requests, [this, sent] (boost::system::error_code const & ec, std::size_t) {
		stats.inc (nano::stat::type::bootstrap_request_ec, nano::to_stat_detail (ec), nano::stat::dir::out);
		sent->set (!ec);
	});

	if (!queued)
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::request_failed);
		co_return request_outcome{ .status = request_outcome::status_t::send_failed };
	}

	stats.inc (nano::stat::type::bootstrap, nano::stat::detail::request);
	stats.inc (nano::stat::type::bootstrap_request, to_stat_detail (type));

	using namespace asio::experimental::awaitable_operators;

	// Phase 1: wait for the wire-send confirmation with a generous grace period. A dying channel can
	// silently drop queued send callbacks; this deadline replaces the old maintenance timeout sweep.
	std::variant<bool, std::monostate> sent_result;
	try
	{
		sent_result = co_await (sent->wait () || clock.sleep_for (config.request_timeout * 4));
	}
	catch (asio::multiple_exceptions const & ex)
	{
		std::rethrow_exception (ex.first_exception ()); // Both operands cancelled, surface the underlying abort
	}
	if (sent_result.index () != 0) // The callback never fired
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::timeout);
		stats.inc (nano::stat::type::bootstrap_timeout, to_stat_detail (type));
		co_return request_outcome{ .status = request_outcome::status_t::timeout };
	}
	if (!std::get<0> (sent_result))
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::request_failed, nano::stat::dir::out);
		co_return request_outcome{ .status = request_outcome::status_t::send_failed };
	}
	stats.inc (nano::stat::type::bootstrap, nano::stat::detail::request_success, nano::stat::dir::out);

	// Phase 2: once the request hit the wire, the peer has a limited time to respond
	std::optional<std::variant<reply, std::monostate>> response_result;
	try
	{
		response_result = co_await (slot->wait () || clock.sleep_for (config.request_timeout));
	}
	catch (asio::multiple_exceptions const & ex)
	{
		std::rethrow_exception (ex.first_exception ());
	}
	if (response_result->index () != 0)
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::timeout);
		stats.inc (nano::stat::type::bootstrap_timeout, to_stat_detail (type));
		co_return request_outcome{ .status = request_outcome::status_t::timeout };
	}

	auto delivered = std::move (std::get<0> (*response_result));
	co_return request_outcome{
		.status = request_outcome::status_t::response,
		.message = std::move (delivered.message),
		.channel = std::move (delivered.channel),
	};
}

asio::awaitable<void> service::wait_limiter (nano::rate_limiter & limiter_a, std::size_t cost)
{
	debug_assert (strand.running_in_this_thread ());
	while (!limiter_a.should_pass (cost))
	{
		co_await clock.sleep_for (25ms); // Refill granularity; virtual time makes this deterministic in tests
	}
}

asio::awaitable<void> service::wait_block_processor ()
{
	co_await nano::async::until (processor_drained, clock, [this] () {
		return block_processor.size (nano::block_source::bootstrap) < config.block_processor_threshold;
	});
}

asio::awaitable<std::shared_ptr<nano::transport::channel>> service::wait_channel ()
{
	debug_assert (strand.running_in_this_thread ());

	while (true)
	{
		auto result = peers.acquire ();
		if (result.status == acquire_status::acquired)
		{
			co_return result.channel;
		}
		co_await nano::async::wait_any (peers_changed.wait (), clock.sleep_for (1s));
	}
}

asio::awaitable<request_outcome::status_t> service::run_pull (std::shared_ptr<nano::transport::channel> channel, blocks_query query, query_source source, nano::async::semaphore::token budget)
{
	debug_assert (strand.running_in_this_thread ());

	auto outcome = co_await request (channel, query, source, std::move (budget));
	if (!outcome)
	{
		// Timeout or send failure: the reserved peer slot stays as an implicit penalty until pool decay
		co_return outcome.status;
	}

	auto const & response = std::get<nano::messages::asc_pull_ack::blocks_payload> (outcome.message->payload);

	stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::blocks);

	auto result = verify (response, query);
	switch (result)
	{
		case verify_result::ok:
		{
			stats.inc (nano::stat::type::bootstrap_verify_blocks, nano::stat::detail::ok);
			stats.add (nano::stat::type::bootstrap, nano::stat::detail::blocks, nano::stat::dir::in, response.blocks.size ());

			// A processed response returns the reserved peer capacity
			peers.release (outcome.channel);
			peers_changed.notify_all ();

			auto blocks = response.blocks;

			// Avoid re-processing the block we already have
			release_assert (blocks.size () >= 1);
			if (blocks.front ()->hash () == query.start.as_block_hash ())
			{
				blocks.pop_front ();
			}

			block_processor.add_many (blocks, nano::block_source::bootstrap, nullptr, [this, account = query.account] (auto result) {
				// It's the last block submitted for this account chain, reset timestamp to allow more requests
				stats.inc (nano::stat::type::bootstrap, nano::stat::detail::timestamp_reset);
				asio::post (strand, [this, account] () {
					accounts.timestamp_reset (account);
					accounts_changed.notify_all ();
				});
			});

			if (source == query_source::database)
			{
				throttle.add (true);
			}
		}
		break;
		case verify_result::nothing_new:
		{
			stats.inc (nano::stat::type::bootstrap_verify_blocks, nano::stat::detail::nothing_new);

			peers.release (outcome.channel);
			peers_changed.notify_all ();

			accounts.priority_down (query.account);
			accounts.timestamp_reset (query.account);
			accounts_changed.notify_all ();

			if (source == query_source::database)
			{
				throttle.add (false);
			}
		}
		break;
		case verify_result::invalid:
		{
			stats.inc (nano::stat::type::bootstrap_verify_blocks, nano::stat::detail::invalid);
			stats.inc (nano::stat::type::bootstrap, nano::stat::detail::invalid_response);
			// No release: the reserved slot penalizes the misbehaving peer until pool decay
		}
		break;
	}

	co_return outcome.status;
}

unsigned service::random (unsigned max)
{
	debug_assert (strand.running_in_this_thread ());
	return std::uniform_int_distribution<unsigned>{ 0, max - 1 }(rng);
}

auto service::resolve_inspect (nano::ledger & ledger, secure::transaction const & tx, nano::block_status const & result, nano::block const & block, nano::block_source source) -> std::optional<inspect_command>
{
	switch (result)
	{
		case nano::block_status::progress:
		{
			// Progress blocks from live traffic don't need further bootstrapping
			if (source != nano::block_source::live)
			{
				bool const is_send = block.is_send ();
				return inspect_command{
					.kind = inspect_command::kind_t::progress,
					.account = block.account (),
					.hash = block.hash (),
					.destination = is_send ? block.destination () : nano::account{ 0 },
					.is_send = is_send,
				};
			}
		}
		break;
		case nano::block_status::gap_source:
		{
			// Prevent malicious live traffic from filling up the blocked set
			if (source == nano::block_source::bootstrap)
			{
				auto const account = block.previous ().is_zero () ? block.account_field ().value () : ledger.any.block_account (tx, block.previous ()).value_or (0);
				auto const source_hash = block.source_field ().value_or (block.link_field ().value_or (0).as_block_hash ());
				if (!account.is_zero () && !source_hash.is_zero ())
				{
					return inspect_command{
						.kind = inspect_command::kind_t::gap_source,
						.account = account,
						.hash = source_hash,
					};
				}
			}
		}
		break;
		case nano::block_status::gap_previous:
		{
			// Prevent live traffic from evicting accounts from the priority list; the half-full
			// checks against live accounts state happen at apply time on the strand
			if (source == nano::block_source::live && block.type () == nano::block_type::state)
			{
				return inspect_command{
					.kind = inspect_command::kind_t::gap_previous,
					.account = block.account_field ().value (),
				};
			}
		}
		break;
		case nano::block_status::gap_epoch_open_pending:
		{
			// Epoch open blocks for accounts that don't have any pending blocks yet
			debug_assert (block.type () == nano::block_type::state); // Only state blocks can have epoch open pending status
			return inspect_command{
				.kind = inspect_command::kind_t::gap_epoch_open,
				.account = block.account_field ().value_or (0),
			};
		}
		break;
		default: // No need to handle other cases
			// TODO: If we receive blocks that are invalid (bad signature, fork, etc.), we should penalize the peer that sent them
			break;
	}
	return std::nullopt;
}

void service::apply_inspect (inspect_command const & command)
{
	debug_assert (strand.running_in_this_thread ());

	switch (command.kind)
	{
		case inspect_command::kind_t::progress:
		{
			// If we've inserted any block in to an account, unmark it as blocked
			accounts.unblock (command.account);
			accounts.priority_up (command.account);

			if (command.is_send)
			{
				accounts.unblock (command.destination, command.hash); // Unblocking automatically inserts account into priority set
				accounts.priority_set (command.destination);
			}
		}
		break;
		case inspect_command::kind_t::gap_source:
		{
			// Mark account as blocked because it is missing the source block
			accounts.block (command.account, command.hash);
		}
		break;
		case inspect_command::kind_t::gap_previous:
		{
			if (!accounts.priority_half_full () && !accounts.blocked_half_full ())
			{
				accounts.priority_set (command.account);
			}
		}
		break;
		case inspect_command::kind_t::gap_epoch_open:
		{
			accounts.priority_erase (command.account);
		}
		break;
	}
}

std::size_t service::compute_throttle_size () const
{
	auto ledger_size = ledger.account_count ();
	size_t target = ledger_size > 0 ? config.throttle_coefficient * static_cast<size_t> (std::log (ledger_size)) : 0;
	size_t min_size = 16;
	return std::max (target, min_size);
}

void service::prioritize (nano::account const & account)
{
	on_strand ([this, account] () {
		accounts.priority_set (account);
		accounts_changed.notify_all ();
	});
}

std::size_t service::priority_size () const
{
	return on_strand ([this] () {
		return accounts.priority_size ();
	});
}

std::size_t service::blocked_size () const
{
	return on_strand ([this] () {
		return accounts.blocked_size ();
	});
}

bool service::prioritized (nano::account const & account) const
{
	return on_strand ([this, account] () {
		return accounts.prioritized (account);
	});
}

bool service::blocked (nano::account const & account) const
{
	return on_strand ([this, account] () {
		return accounts.blocked (account);
	});
}

auto service::info () const -> account_sets_index::info_t
{
	return on_strand ([this] () {
		return accounts.info ();
	});
}

nano::container_info service::container_info () const
{
	return on_strand ([this] () {
		auto collect_limiters = [this] () {
			nano::container_info info;
			info.put ("total", limiter.size ());
			info.put ("database", database_limiter.size ());
			info.put ("frontiers", frontiers_limiter.size ());
			return info;
		};

		nano::container_info info;
		info.put ("tags", requests.size ());
		info.put ("throttle", throttle.size ());
		info.put ("throttle_successes", throttle.successes ());
		info.add ("accounts", accounts.container_info ());
		info.add ("database_scan", database_scan.container_info ());
		info.add ("frontiers", frontier->container_info ());
		info.add ("workers", workers.container_info ());
		info.add ("peers", peers.container_info ());
		info.add ("limiters", collect_limiters ());
		return info;
	});
}
}
