# Code samples: Harbor, Portcullis, Loom — and the tests they unlock

Companion to `transport_redesign_proposal.md` and `transport_redesign_designs.md`. All samples are illustrative sketches in the codebase's style, anchored to real current code (quoted from develop @ 28640d188). Two recurring examples are shown through each design's lens — the inbound receive path and the outbound bandwidth wait — followed by the test catalogue.

## 0. The code as it is today (the baseline)

The receive path, [tcp_server.cpp:285-289](../nano/node/transport/tcp_server.cpp) — four reaches through the node god-object in one call:

```cpp
auto result = nano::deserialize_message (payload_buffer, header,
node.network_params.network,
&node.network.filter,
&node.block_uniquer,
&node.vote_uniquer);
```

Message delivery, [tcp_server.cpp:192-193](../nano/node/transport/tcp_server.cpp) — hardwired to a concrete node component:

```cpp
bool added = node.message_processor.put (std::move (message), channel);
node.stats.inc (nano::stat::type::tcp_server, added ? nano::stat::detail::message_queued : nano::stat::detail::message_dropped);
```

The bandwidth wait, [tcp_channel.cpp:158-171](../nano/node/transport/tcp_channel.cpp) — a real 100ms timer poll against a node member:

```cpp
const size_t bandwidth_chunk = 128 * 1024;
while (allocated_bandwidth < size)
{
	if (node.outbound_limiter.should_pass (bandwidth_chunk, type))
	{
		allocated_bandwidth += bandwidth_chunk;
	}
	else
	{
		node.stats.inc (nano::stat::type::tcp_channel_wait, nano::stat::detail::wait_bandwidth, nano::stat::dir::out);
		co_await nano::async::sleep_for (100ms);
	}
}
```

Because of `node.stats`, `node.config`, `node.outbound_limiter`, `node.message_processor`, none of these three fragments can execute without a fully started node — ledger, store, wallets, thread pool. That is the whole testability problem in miniature.

---

## 1. Harbor — the same code behind a context and two ports

Harbor changes *who the code talks to*, not what it does. Every `node.` access is replaced by a field of an injected `transport_context`; the two genuinely node-bound operations (message delivery, lifecycle events) become abstract ports.

```cpp
// nano/transport/transport_context.hpp
namespace nano::transport
{
struct transport_context
{
	asio::io_context & io_ctx;
	nano::network_constants const & network; // nano/lib — deliberately NOT network_params (nano/secure)
	nano::stats & stats;
	nano::logger & logger;
	nano::transport::tcp_config const & config;
	nano::transport::bandwidth_limiter & outbound_limiter;
	nano::async::timer_service & clock; // grafted from Loom in the final proposal

	// Wired by the node after construction, release_assert'ed in transport_service::start ()
	nano::network_filter * filter{};
	nano::block_uniquer * block_uniquer{};
	nano::vote_uniquer * vote_uniquer{};
	nano::transport::message_sink * sink{};
	nano::transport::channel_events * events{};

	struct
	{
		bool disable_tcp_realtime{};
		bool disable_max_peers_per_ip{};
		bool disable_max_peers_per_subnetwork{};
		bool allow_local_peers{};
	} flags;
};

// nano/transport/ports.hpp — the ONLY library -> node edges.
// Contract: invoked outside library mutexes, must not block, re-entering the service is legal.
class message_sink
{
public:
	virtual bool on_message (std::unique_ptr<nano::messages::message>, std::shared_ptr<channel>) = 0;
	virtual void process_inbound (nano::messages::message const &, std::shared_ptr<channel>) = 0; // loopback path
};

class channel_events
{
public:
	virtual void on_connected (std::shared_ptr<channel>) = 0;
	virtual void on_disconnected (std::shared_ptr<channel>)
	{
	} // additive, default no-op
};
}
```

The receive path becomes (same logic, line for line):

```cpp
// nano/transport/tcp_server.cpp — Harbor
if (header.network != ctx.network.current_network) // was node.config.network_params.network
{
	co_return nano::deserialize_message_result{ nullptr, nano::deserialize_message_status::invalid_network };
}
...
auto result = nano::deserialize_message (payload_buffer, header,
ctx.network,
ctx.filter,
ctx.block_uniquer,
ctx.vote_uniquer);
```

```cpp
bool added = ctx.sink->on_message (std::move (message), channel); // was node.message_processor.put
ctx.stats.inc (nano::stat::type::tcp_server, added ? nano::stat::detail::message_queued : nano::stat::detail::message_dropped);
```

Handshake identity enters as plain values, so the library never links the ledger:

```cpp
// nano/transport/handshake.hpp
struct handshake_identity
{
	nano::keypair node_id; // nano/lib/keypair.hpp
	nano::block_hash genesis; // plain value — no nano_secure dependency
	std::function<nano::node_capabilities_flags ()> capabilities; // matches node::get_capabilities being a live call
};
```

Production wiring in `node.cpp` shrinks to adapters of ~5 lines each:

```cpp
// node.cpp (anonymous namespace)
struct node_message_sink final : nano::transport::message_sink
{
	nano::node & node;
	bool on_message (std::unique_ptr<nano::messages::message> message, std::shared_ptr<nano::transport::channel> channel) override
	{
		return node.message_processor.put (std::move (message), channel);
	}
	void process_inbound (nano::messages::message const & message, std::shared_ptr<nano::transport::channel> channel) override
	{
		node.inbound (message, channel);
	}
};
```

And the payoff — the entire transport stack constructible in a test with **zero node**:

```cpp
// transport_test/fixtures.hpp — the ~60-line fixture replacing nano::test::system
struct transport_fixture
{
	asio::io_context io_ctx;
	nano::stats stats{ nano::stats_config{} };
	nano::logger logger;
	nano::transport::tcp_config config{ nano::dev::network_params.network };
	nano::transport::bandwidth_limiter limiter{ nano::transport::bandwidth_limiter_config{} };
	nano::test::manual_timer_service clock; // step time by hand (see §4)
	nano::test::recording_sink sink; // captures (message, channel) pairs
	nano::test::recording_events events;
	nano::transport::transport_context ctx{ io_ctx, nano::dev::network_params.network, stats, logger, config, limiter, clock };
	nano::transport::transport_service service;

	transport_fixture () :
		service{ ctx, nano::test::dev_identity (), /* port */ 0 }
	{
		ctx.sink = &sink;
		ctx.events = &events;
		service.start ();
	}
};
```

---

## 2. Portcullis — the same destination as a sequence of one-seam diffs

Portcullis's contribution is not different end-state code — it is the *shape of each PR*. One dependency edge inverted per diff, byte-identical behavior, existing tests as the oracle. The bandwidth-wait seam PR is literally this:

```diff
 	while (allocated_bandwidth < size)
 	{
-		if (node.outbound_limiter.should_pass (bandwidth_chunk, type))
+		if (ctx.outbound_limiter.should_pass (bandwidth_chunk, type))
 		{
 			allocated_bandwidth += bandwidth_chunk;
 		}
 		else
 		{
-			node.stats.inc (nano::stat::type::tcp_channel_wait, nano::stat::detail::wait_bandwidth, nano::stat::dir::out);
+			ctx.stats.inc (nano::stat::type::tcp_channel_wait, nano::stat::detail::wait_bandwidth, nano::stat::dir::out);
 			co_await nano::async::sleep_for (100ms);
 		}
 	}
```

Its signature move on the handshake: the state machine moves into the library *now*, the crypto stays on `nano::network` *for now*, behind a three-method interface that doubles as the revert seam:

```cpp
// nano/transport/handshake_provider.hpp — implemented by nano::network (methods already exist
// verbatim at network.cpp:728-803; this PR only adds `override`)
class handshake_provider
{
public:
	virtual std::optional<nano::messages::node_id_handshake::query_payload> prepare_handshake_query (nano::endpoint const &) = 0;
	virtual nano::messages::node_id_handshake::response_payload prepare_handshake_response (nano::messages::node_id_handshake::query_payload const &, nano::messages::handshake_version) const = 0;
	virtual bool verify_handshake_response (nano::messages::node_id_handshake::response_payload const &, nano::endpoint const &) = 0;
};
```

`tcp_server::perform_handshake` stops *being* the state machine and starts *driving* one — IO stays in the coroutine, decisions move to a pure class:

```cpp
// nano/transport/tcp_server.cpp — after the FSM extraction
asio::awaitable<handshake_status> nano::transport::tcp_server::perform_handshake ()
{
	nano::transport::handshake_driver driver{ *ctx.handshake, ctx.network, handshake_role::server, get_remote_endpoint (), ctx.flags.disable_tcp_realtime };

	co_await apply (driver.begin ());
	while (!driver.is_terminal ())
	{
		auto [message, status] = co_await receive_message ();
		auto handshake = dynamic_cast<nano::messages::node_id_handshake *> (message.get ());
		if (!handshake)
		{
			co_return handshake_status::abort; // the 764a0d113 lesson, now structural
		}
		co_await apply (driver.on_message (*handshake));
	}
	co_return driver.result (); // realtime / abort
}

// apply(): send_request/send_response -> co_write; promote_realtime -> create channel; abort -> close
```

And the migration mechanics that keep ~87 consumer files out of the conflict-magnet move PR — a forwarding shim, deleted later in its own mechanical PR:

```cpp
// nano/node/transport/tcp_channel.hpp — after the git mv (entire file)
#pragma once
#include <nano/transport/tcp_channel.hpp>
```

What Portcullis buys in tests *during* the migration: each seam PR adds tests against the **current** behavior before anything moves — e.g. the WRR queue tests and admission boundary tests land against develop unchanged, so the later moves have a pinned oracle.

---

## 3. Loom — different code, not just different wiring

Loom replaces the socket/server/channel triple with one `connection` owning everything on one strand, inside one cancellation scope, with a deadline per state and an injected clock. The runtime is the piece the final proposal adopts (as `timer_service`); the connection rewrite is the deferred "phase 6".

```cpp
// nano/lib/async/runtime.hpp
class runtime
{
public:
	static runtime make_threaded (unsigned io_threads, nano::logger &); // production
	static runtime make_deterministic (); // tests: no threads, simulated clock

	nano::async::strand make_strand ();
	nano::async::timer_service & timers (); // now() + awaitable sleep; sim version fires on advance()
	void poll (); // deterministic mode: run all ready handlers
	void advance (std::chrono::milliseconds); // move sim clock, fire due timers, poll
	void stop_and_join (); // the ONLY synchronous join in the system
};
```

```cpp
// nano/transport/connection.hpp — one connection, one strand, one scope
asio::awaitable<void> nano::transport::connection::run ()
{
	if (auto ec = co_await with_deadline (timers, strand, timers.now () + config.connect_timeout, do_connect ()))
	{
		co_return finish (ec);
	}
	if (auto ec = co_await with_deadline (timers, strand, timers.now () + config.handshake_timeout, do_handshake ()))
	{
		co_return finish (ec);
	}

	scope.spawn (reader_loop ()); // bytes -> frames -> messages -> sink.on_message
	scope.spawn (writer_loop ()); // gate.wait -> queue.next_batch -> bandwidth.acquire -> stream.write
	scope.spawn (watchdog ()); // idle/silent deadlines + keepalive scheduling, all off timers.now()

	co_await scope.join (); // first child to exit cancels its siblings — structurally
	stream->close (); // idempotent, non-blocking
	flush_pending_callbacks (asio::error::operation_aborted);
	events.on_disconnected (handle); // fires exactly once
}
```

The bandwidth wait stops polling — acquire is awaitable, woken by token refills, debited against the message's own traffic type, refunded on cancellation:

```cpp
// writer_loop body (sketch)
co_await bandwidth.acquire (buffer.size (), type); // event-driven; no 100ms sleep
auto result = co_await stream->co_write_all (buffer);
```

And the test double that makes kernel buffers irrelevant:

```cpp
// nano/transport/pipe_stream.hpp (ships in the final proposal as a test utility)
auto [client, server] = nano::transport::pipe_stream::make_pair (runtime);
server->script_partial_read (5); // next read delivers only 5 bytes
server->script_stall (); // then hang until advance()
server->script_error (asio::error::connection_reset); // then fail
```

---

## 4. The test catalogue — edge cases that become writable

Everything below runs in a `transport_test` target with no ledger, no store, no wallets, no `nano::test::system`, in milliseconds instead of 10–15s `ASSERT_TIMELY` polls. Labels show which design contributes the enabling piece; "synthesis" = the final proposal (Harbor boundary + Loom clock/pipe + Portcullis staging).

### 4.1 Handshake protocol edge cases — pure, no IO at all (Harbor/Portcullis FSM)

Today: only testable by byte-bashing a live listener on a full node; the *client* role has zero tests.

```cpp
TEST (handshake_driver, duplicate_query_exhausts_message_budget)
{
	nano::test::handshake_fixture fixture; // identity + syn_cookies + dev network_constants
	nano::transport::handshake_driver driver{ fixture.ctx, handshake_role::server, fixture.remote };

	auto first = driver.on_message (fixture.make_query ());
	ASSERT_TRUE (has_event<send_response> (first));

	auto second = driver.on_message (fixture.make_query ()); // hostile peer re-sends the query
	ASSERT_TRUE (has_event<abort_connection> (second)); // 2-message budget enforced
	ASSERT_TRUE (driver.is_terminal ());
}

TEST (handshake_driver, client_role_full_exchange) // first test of the initiator side EVER
{
	nano::transport::handshake_driver client{ fixture.ctx, handshake_role::client, fixture.remote };
	auto begin = client.begin ();
	ASSERT_TRUE (has_event<send_request> (begin));
	auto events = client.on_message (fixture.make_response_to (begin));
	ASSERT_TRUE (has_event<promote_realtime> (events));
}
```

With stage-2 internalization (real crypto in-library), two previously-untestable behaviors become unit tests:

```cpp
TEST (handshake, cookie_fetch_and_erase_is_one_shot)
{
	// a valid response verifies once; replaying the same signed response must fail
	ASSERT_TRUE (fixture.verify (response));
	ASSERT_FALSE (fixture.verify (response)); // cookie was erased on first verify
}

TEST (handshake, v1_response_skips_genesis_check) // network.cpp:738-743, encoded as a test at last
```

### 4.2 The NAT-correlated cookie-exhaustion failure (synthesis; Judge 1's flagged regression)

The silent mainnet failure mode the migration must pin: un-purged cookies from failed dials wedge all future handshakes to that IP.

```cpp
TEST (handshake, cookie_exhaustion_recovers_after_purge)
{
	transport_fixture fixture;
	auto & cookies = fixture.service.syn_cookies ();

	for (size_t i = 0; i < fixture.config.admission.max_peers_per_ip; ++i)
	{
		ASSERT_TRUE (cookies.assign (fixture.endpoint_in_same_subnet (i)).has_value ()); // failed dials accumulate
	}
	ASSERT_FALSE (cookies.assign (fixture.endpoint_in_same_subnet (99)).has_value ()); // wedged — prepare_handshake_query would return nullopt

	fixture.clock.advance (nano::transport::syn_cookie_cutoff + 1s);
	fixture.service.purge (fixture.clock.now ());

	ASSERT_TRUE (cookies.assign (fixture.endpoint_in_same_subnet (99)).has_value ()); // reclaimed
}
```

### 4.3 Timeout boundaries without sleeping (Harbor's `checkup_tick` + Loom's clock)

Today: `core_test/socket.cpp` sleeps real seconds and dials unroutable IPs to provoke connect timeouts; eviction tests are `DISABLED_`.

```cpp
TEST (tcp_socket, silent_timeout_exact_boundary)
{
	transport_fixture fixture;
	auto socket = fixture.make_connected_socket_pair ().first;

	socket->checkup_tick (fixture.config.silent_connection_tolerance - 1s);
	ASSERT_TRUE (socket->alive ()); // one tick before the cutoff: alive

	socket->checkup_tick (2s);
	ASSERT_FALSE (socket->alive ()); // past the cutoff: dead — and the 5s quantization is documented here
	ASSERT_TRUE (socket->has_timed_out ());
}

TEST (tcp_listener, handshake_timeout_sweeps_undecided_connections)
{
	transport_fixture fixture;
	fixture.connect_raw_socket_that_never_handshakes ();
	fixture.clock.advance (fixture.config.handshake_timeout + 1s); // fires the sweep's timer deterministically
	fixture.io_ctx.poll ();
	ASSERT_EQ (fixture.service.listener.connection_count (), 0); // evicted, no wall-clock wait
}
```

### 4.4 Framing and hostile input (Loom's `pipe_stream`; the 4e1414db2 family)

Today: partial reads can only be provoked through real kernel buffers; the fuzzer doesn't compile.

```cpp
TEST (tcp_server, eof_mid_payload_terminates_cleanly)
{
	transport_fixture fixture;
	auto [near, far] = nano::transport::pipe_stream::make_pair (fixture.runtime);
	auto server = fixture.make_server (std::move (near));

	auto bytes = nano::test::serialize (nano::test::make_publish ());
	far->write (std::span{ bytes }.first (nano::messages::message_header::size + 3)); // header + 3 payload bytes
	far->close (); // EOF mid-payload

	fixture.io_ctx.poll ();
	ASSERT_FALSE (server->alive ()); // terminated, no hang, no crash, no partial message delivered
	ASSERT_TRUE (fixture.sink.messages.empty ());
}

TEST (tcp_server, publish_with_undeserializable_block_is_rejected) // 4e1414db2 as a permanent unit test
{
	far->write (nano::test::publish_header_with_truncated_block ());
	fixture.io_ctx.poll ();
	ASSERT_EQ (fixture.stats.count (nano::stat::type::tcp_server_message_error, nano::stat::detail::invalid_publish_message), 1);
	// before the fix this was a remotely-triggerable null dereference
}

TEST (tcp_server, header_delivered_one_byte_at_a_time) // scripted partial reads
{
	far->script_partial_read (1); // every read returns exactly 1 byte
	far->write (nano::test::serialize (nano::test::make_keepalive ()));
	fixture.io_ctx.poll ();
	ASSERT_EQ (fixture.sink.messages.size (), 1); // reassembly across 8+ partial reads works
}
```

### 4.5 Prioritization fairness and queue overflow (landable TODAY, zero refactor)

`tcp_channel_queue` is already dependency-free and has zero tests. These pin the WRR semantics before anything moves:

```cpp
TEST (tcp_channel_queue, wrr_interleaving_under_saturation)
{
	nano::transport::tcp_channel_queue queue;
	fill_to_max (queue, traffic_type::generic); // priority weight 4
	fill_to_max (queue, traffic_type::block_broadcast); // priority weight 1

	auto first_ten = drain (queue, 10);
	// documents the actual interleave: high-priority traffic dominates but never starves the low band
	ASSERT_EQ (count (first_ten, traffic_type::generic), 8);
	ASSERT_EQ (count (first_ten, traffic_type::block_broadcast), 2);
}

TEST (tcp_channel_queue, overflow_drops_without_callback) // today's semantics, now documented
{
	for (size_t i = 0; i < tcp_channel_queue::full_size; ++i)
	{
		queue.push (traffic_type::vote, entry ());
	}
	ASSERT_TRUE (queue.full (traffic_type::vote));
	// push beyond full: dropped, callback never invoked — the wart bootstrap_context compensates for
}
```

### 4.6 Backpressure against the limiter (synthesis; replaces the vacuous inproc test)

Today's `bandwidth_limiter_with_burst` passes trivially because inproc channels never consult the limiter.

```cpp
TEST (tcp_channel, send_waits_for_bandwidth_then_proceeds)
{
	transport_fixture fixture;
	fixture.limiter_stub.deny_next (3); // stub: refuse 3 should_pass calls, then allow

	auto channel = fixture.make_tcp_channel_pair ().first;
	ASSERT_TRUE (channel->send (nano::test::make_vote_message (), traffic_type::vote));

	fixture.clock.advance (100ms); // first retry
	fixture.clock.advance (100ms); // second
	fixture.clock.advance (100ms); // third — limiter now allows
	fixture.io_ctx.poll ();

	ASSERT_EQ (fixture.stats.count (nano::stat::type::tcp_channel_wait, nano::stat::detail::wait_bandwidth), 3);
	ASSERT_EQ (fixture.received_on_peer (), 1); // delayed, not dropped
}
```

### 4.7 The deadlock class, pinned (synthesis: the `reentrant_ports` fixture)

Today reproducible only as the full-node `purge_bootstrap_deadlock` system test. This pins the "ports outside locks" contract — the 96aa784ae / 0357518b8 / 6a3cd8f1f class — as a sub-second test:

```cpp
struct reentrant_sink final : nano::transport::message_sink
{
	nano::transport::transport_service & service;
	bool on_message (std::unique_ptr<nano::messages::message> message, std::shared_ptr<nano::transport::channel> channel) override
	{
		service.exclude (channel); // re-enter the service from inside the port callback
		auto list = service.channels.list (); // and take the registry lock
		return true;
	}
	...
};

TEST (transport_service, reentrant_ports_never_deadlock)
{
	transport_fixture fixture{ /* sink = */ reentrant_sink{} };
	fixture.spawn_fake_channels (64);

	nano::test::watchdog watchdog{ 5s }; // fail loudly instead of hanging CI
	std::vector<std::thread> threads;
	for (int i = 0; i < 4; ++i)
	{
		threads.emplace_back ([&] { fixture.service.purge (fixture.clock.now ()); });
		threads.emplace_back ([&] { fixture.deliver_messages_to_random_channels (); });
	}
	fixture.service.stop (); // swap-then-close teardown under concurrent reentry
	join_all (threads);
}
```

### 4.8 Admission and the wedge-proof accept loop (Harbor/Portcullis; the ade6f224d class)

```cpp
TEST (tcp_listener, accepts_resume_after_saturation) // the listener must never stop accepting permanently
{
	transport_fixture fixture; // admission.max_inbound_connections = 8 via dev config
	auto held = fixture.open_raw_connections (8);
	auto rejected = fixture.open_raw_connection ();
	ASSERT_TRUE (fixture.eventually_closed (rejected)); // accept-then-drop, not stop-accepting

	held.front ().close ();
	fixture.io_ctx.poll ();
	ASSERT_TRUE (fixture.open_raw_connection_succeeds ()); // slot reclaimed, accepting again
}

TEST (admission, per_ip_limit_boundaries) // table-driven: 0 / at-limit / above, per direction, per cap
{
	// also encodes the current `>` vs `>=` off-by-one (tcp_listener.cpp:272/281) as
	// expected-current-behavior, so fixing it later is a deliberate, visible diff
}
```

### 4.9 Lifetime under sanitizers (synthesis; cheap fixtures make ASAN/TSAN sweeps affordable)

```cpp
TEST (transport_service, destroy_mid_io) // service destroyed while reads/writes are in flight
TEST (tcp_channel, last_handle_dropped_from_port_callback) // the release-build hazard at tcp_channel.cpp:26-29
TEST (tcp_channel, concurrent_send_from_8_threads) // TSAN: single-writer invariant over a socketpair
TEST (tcp_listener, cancel_dial_mid_connect) // attempt cancelled between connect and handshake
```

### 4.10 Eviction without noise (un-DISABLEs `peer_container.DISABLED_tcp_channel_cleanup_works`)

```cpp
TEST (tcp_channels, purge_evicts_by_last_packet_sent)
{
	transport_fixture fixture;
	auto channel = fixture.make_registered_channel ();
	fixture.clock.advance (fixture.config.cleanup_cutoff + 1s); // no background keepalives to race against
	fixture.service.purge (fixture.clock.now ());
	ASSERT_EQ (fixture.service.channels.size (), 0);
	ASSERT_EQ (fixture.events.disconnected.size (), 1); // and the event fired exactly once
}
```

### Only Loom's deferred rewrite buys these two

Worth naming because they motivate the §9 "phase 6" open question in the proposal:

```cpp
TEST (connection, dropping_last_handle_on_io_thread_never_blocks) // structural, not conventional
TEST (runtime, full_connection_lifecycle_under_simulated_time) // zero real threads, zero real sockets, 100% deterministic schedule
```

Under the synthesis, the first is *pinned* by the reentrant-ports fixture but the blocking close engine survives; the second is approximated by manual-clock + pipe_stream but the io_context still schedules. Loom's runtime makes both literal.

---

## Summary table

| Edge case | Today | Enabled by |
|---|---|---|
| Handshake budget/duplicate-query/abort paths | full node + byte-bashing + stat polling | pure FSM (all three designs) |
| Client-side handshake | **untested** | pure FSM, `handshake_role::client` |
| Cookie one-shot verify, v1 genesis bypass | **untestable** (crypto needs node) | Harbor stage-2 internalization |
| Cookie exhaustion / purge regression | **untestable**, silent mainnet failure | synthesis `purge()` + manual clock |
| Timeout exact boundaries | real sleeps, unroutable-IP hacks | `checkup_tick` + `timer_service` |
| Partial reads / EOF-mid-payload / null-block | kernel-buffer luck; fuzzer bit-rotted | `pipe_stream` + single parser + revived fuzzer |
| WRR fairness, starvation, overflow-drop | **zero tests** despite zero dependencies | landable today (Portcullis step 2) |
| Bandwidth backpressure | vacuous (inproc bypasses limiter) | injected limiter stub + clock |
| Close-under-lock deadlock | one full-node system test | reentrant-ports fixture (pins); Loom (eliminates) |
| Accept-loop wedge at saturation | **untested** (caused fd leaks in prod) | bare listener + raw connections |
| Eviction cutoffs | `DISABLED_` (racy on full node) | registry + manual clock, no keepalive noise |
| Lifetime/UAF sweeps under ASAN/TSAN | too expensive on full nodes | cheap fixtures |
