# Extract Transport Library with Dependency Inversion

## Context

Transport code lives in `nano/node/transport/` (34 files) and is tightly coupled to the node -- every `.cpp` includes `nano/node/node.hpp` and accesses `node.stats`, `node.io_ctx`, `node.logger`, `node.config`, `node.flags`, `node.network`, `node.message_processor`, etc. The goal is to move this code into a standalone `nano/transport/` library with the dependency inverted: **node depends on transport, not the other way around**.

A `nano/transport/` directory already exists with a minimal `transport_service` (holds io_ctx, stats, logger, network_params + thread_runner). This plan extends it.

## Architecture

### Two new core types

**`transport_context`** -- pure dependency container that all transport components receive:
- Dependency refs: `io_ctx`, `stats`, `logger`, `network_params`, `tcp_config`, `bandwidth_limiter`
- Pointers (set by node): `network_filter*`, `block_uniquer*`, `vote_uniquer*`, `handshake_provider*`
- Callbacks (wired by node): `on_message`, `create_channel`, `is_excluded`, `connect`, `bootstrap_count`, `random_fill`, `on_channel_connected`, `on_socket_connected`
- Flags: `disable_tcp_realtime`, `disable_bootstrap_listener`, `disable_max_peers_per_ip`, `disable_max_peers_per_subnetwork`, `allow_local_peers`

**`transport_service`** -- top-level orchestrator:
- Owns/creates `io_context` + `thread_runner`
- Creates and owns `transport_context`
- Owns `tcp_service` (renamed from `tcp_channels`)
- Owns `tcp_listener`
- Provides `start()` / `stop()` lifecycle

### Other design decisions
- **Handshake**: Abstract `handshake_provider` interface in `nano/transport/` with 3 virtual methods (`prepare_query`, `prepare_response`, `verify_response`). `nano::network` implements it. Pointer stored in `transport_context`.
- **`channel::owner()`**: Replace `shared_ptr<node>` return with opaque `void*` owner ID set at construction. Debug assert in `node::inbound()` compares `channel->owner() == this`.
- **`tcp_channels`**: Renamed to `tcp_service`, moved to `nano/transport/`. `tcp_listener` stays separate.
- **`bandwidth_limiter`**: Moved to `nano/transport/`. Construction config becomes a plain struct (node builds it from `node_config`/`node_flags`).
- **Migration**: Big-bang include path updates (no compatibility forwarding headers).
- **`loopback`/`fake`/`test_channel`**: Move to `nano/transport/` (they become trivial after base class refactoring).
- **`inproc::channel`**: Keep in `nano/node/` (holds refs to two nodes, test-only).

### Target dependency graph
```
nano_lib -> nano_messages -> nano_transport -> node
```

## Implementation Phases

### Phase 1: Establish transport_context and move zero-dependency files

**1a. Define `transport_context` in `nano/transport/transport_context.hpp`**

```cpp
struct transport_context {
    asio::io_context & io_ctx;
    nano::network_params const & network_params;
    nano::stats & stats;
    nano::logger & logger;
    tcp_config const & tcp_config;
    bandwidth_limiter & outbound_limiter;

    nano::network_filter * network_filter{ nullptr };
    nano::block_uniquer * block_uniquer{ nullptr };
    nano::vote_uniquer * vote_uniquer{ nullptr };
    handshake_provider * handshake{ nullptr };

    struct {
        bool disable_tcp_realtime{};
        bool disable_bootstrap_listener{};
        bool disable_max_peers_per_ip{};
        bool disable_max_peers_per_subnetwork{};
        bool allow_local_peers{};
    } flags;

    // Callbacks wired by node at construction
    std::function<bool(std::unique_ptr<nano::messages::message>, std::shared_ptr<channel>)> on_message;
    std::function<void(std::shared_ptr<channel>)> on_channel_connected;
    std::function<bool(boost::asio::ip::address const &)> is_excluded;
    std::function<bool(boost::asio::ip::address, uint16_t)> connect;
    std::function<size_t()> bootstrap_count;
    std::function<void(std::array<nano::endpoint, 8> &)> random_fill;
    std::function<bool(nano::endpoint const &)> is_not_a_peer;
};
```

**1b. Define `handshake_provider` interface in `nano/transport/handshake_provider.hpp`**
- 3 pure virtual methods: `prepare_query`, `prepare_response`, `verify_response`

**1c. Move `bandwidth_limiter` to `nano/transport/`**
- From: `nano/node/bandwidth_limiter.hpp/cpp`
- Change constructor to take `bandwidth_limiter_config` directly (plain struct with `generic_limit`, `generic_burst_ratio`, `bootstrap_limit`, `bootstrap_burst_ratio`)
- Node builds the config from `node_config`/`node_flags` and passes it in

**1d. Move zero-dependency files to `nano/transport/`**
- `traffic_type.hpp/cpp` -- pure enum, no node deps
- `tcp_config.hpp/cpp` -- config struct, depends only on `nano/lib/config.hpp`
- `common.hpp` (socket_type, socket_endpoint, buffer_drop_policy enums)
- `fwd.hpp` -- forward declarations (merge with existing `nano/transport/fwd.hpp`)
- `formatting.hpp` -- fmt formatter for channel

Files to create:
- `nano/transport/transport_context.hpp`
- `nano/transport/handshake_provider.hpp`

Files to move:
- `nano/node/bandwidth_limiter.hpp/cpp` -> `nano/transport/bandwidth_limiter.hpp/cpp`
- `nano/node/transport/traffic_type.hpp/cpp` -> `nano/transport/traffic_type.hpp/cpp`
- `nano/node/transport/tcp_config.hpp/cpp` -> `nano/transport/tcp_config.hpp/cpp`
- `nano/node/transport/common.hpp` -> `nano/transport/common.hpp` (merge with existing)
- `nano/node/transport/fwd.hpp` -> `nano/transport/fwd.hpp` (merge with existing)
- `nano/node/transport/formatting.hpp` -> `nano/transport/formatting.hpp`

### Phase 2: Refactor and move channel base class + tcp_socket

**2a. Refactor `channel` base class**
- Replace `nano::node& node` with `transport_context& ctx`
- Constructor: `channel(transport_context&, void* owner_id)`
- `send()`: `ctx.stats.inc(...)` instead of `node.stats.inc(...)`
- `owner()`: returns `void*` owner_id
- Remove `#include <nano/node/bandwidth_limiter.hpp>` (bandwidth_limiter is now in nano/transport/)
- Move to `nano/transport/channel.hpp/cpp`

**2b. Refactor `tcp_socket`**
- Replace `nano::node&` with `transport_context&`
- `node.io_ctx` -> `ctx.io_ctx`
- `node.stats` -> `ctx.stats`
- `node.logger` -> `ctx.logger`
- `node.config.tcp.*` -> `ctx.tcp_config.*`
- Move to `nano/transport/tcp_socket.hpp/cpp`

**2c. Move `transport.hpp/cpp`** (address utility functions)
- Remove gratuitous `#include <nano/node/node.hpp>` from .cpp (code doesn't actually use node)
- Remove `#include <nano/node/bandwidth_limiter.hpp>` from header
- Move to `nano/transport/transport.hpp/cpp` (or rename to `nano/transport/address_utils.hpp`)

**2d. Move `block_deserializer.hpp/cpp`**
- Already has no node dependency (only depends on tcp_socket and nano/lib)
- Move to `nano/transport/block_deserializer.hpp/cpp`

**2e. Move `message_deserializer.hpp/cpp`**
- Already takes deps individually in constructor (network_constants, network_filter, block_uniquer, vote_uniquer)
- No changes needed to the class itself
- Move to `nano/transport/message_deserializer.hpp/cpp`

Files to modify and move:
- `nano/node/transport/channel.hpp/cpp` -> `nano/transport/channel.hpp/cpp`
- `nano/node/transport/tcp_socket.hpp/cpp` -> `nano/transport/tcp_socket.hpp/cpp`
- `nano/node/transport/transport.hpp/cpp` -> `nano/transport/transport.hpp/cpp`
- `nano/node/transport/block_deserializer.hpp/cpp` -> `nano/transport/block_deserializer.hpp/cpp`
- `nano/node/transport/message_deserializer.hpp/cpp` -> `nano/transport/message_deserializer.hpp/cpp`

### Phase 3: Refactor and move tcp_channel + tcp_server

**3a. Refactor `tcp_channel`**
- Replace `node.` with `ctx.` throughout
- `node.io_ctx` -> `ctx.io_ctx` (strand creation)
- `node.stats` -> `ctx.stats`
- `node.outbound_limiter.should_pass(...)` -> `ctx.outbound_limiter.should_pass(...)`
- Move to `nano/transport/tcp_channel.hpp/cpp`

**3b. Refactor `tcp_server`** (heaviest coupling)
- Replace `nano::node&` with `transport_context&`
- Stats/logger/io_ctx: mechanical `node.` -> `ctx.` replacement
- Network params: `node.config.network_params.network.*` -> `ctx.network_params.network.*`
- Deserialization: `node.network.filter` -> `*ctx.network_filter`, `node.block_uniquer` -> `*ctx.block_uniquer`, `node.vote_uniquer` -> `*ctx.vote_uniquer`
- Message delivery: `node.message_processor.put(msg, channel)` -> `ctx.on_message(msg, channel)`
- Handshake: `node.network.prepare_handshake_query(...)` -> `ctx.handshake->prepare_query(...)`, same for response/verify
- Channel creation: `node.network.tcp_channels.create(socket, server, node_id, flags)` -> callback or direct call to `tcp_service` via context
- Flags: `node.flags.disable_tcp_realtime` -> `ctx.flags.disable_tcp_realtime`
- Bootstrap: `node.tcp_listener.bootstrap_count()` -> `ctx.bootstrap_count()`
- Move to `nano/transport/tcp_server.hpp/cpp`

### Phase 4: Refactor and move tcp_listener + tcp_service

**4a. Refactor `tcp_listener`**
- Replace `nano::node&` with `transport_context&`
- All stats/logger/io_ctx/config/flags: mechanical replacement
- `node.network.excluded_peers.check(ip)` -> `ctx.is_excluded(ip)`
- Connection limits from config -> `ctx.flags.*` / `tcp_config` or transport_context fields
- Observer notifications -> `ctx.on_socket_connected(...)` or similar callback
- Move to `nano/transport/tcp_listener.hpp/cpp`

**4b. Refactor `tcp_channels` -> `tcp_service`**
- Rename class from `tcp_channels` to `tcp_service`
- Replace `nano::node&` with `transport_context&`
- `node.network.not_a_peer(...)` -> `ctx.is_not_a_peer(...)`
- `node.network.excluded_peers.check(...)` -> `ctx.is_excluded(...)`
- `node.observers.channel_connected.notify(channel)` -> `ctx.on_channel_connected(channel)`
- `node.tcp_listener.connect(...)` -> `ctx.connect(...)`
- `node.network.random_fill(...)` -> `ctx.random_fill(...)`
- Peer limits from config/flags: mechanical replacement
- Move to `nano/transport/tcp_service.hpp/cpp`

### Phase 5: Move remaining channel types + special files

**5a. Move `test_channel.hpp/cpp`** to `nano/transport/`
- Trivial: uses base class constructor only, no direct node deps

**5b. Move `fake.hpp/cpp`** to `nano/transport/`
- `send_impl` posts callback to `ctx.io_ctx` instead of `node.io_ctx`
- Constructor endpoint passed as parameter instead of using node

**5c. Move `loopback.hpp/cpp`** to `nano/transport/`
- Constructor: pass endpoint and node_id as params instead of reading from node
- `send_impl`: `node.inbound(msg, channel)` -> `ctx.on_message(msg, channel)`
- `node.io_ctx` -> `ctx.io_ctx` for callback posting

**5d. Keep `inproc.hpp/cpp`** in `nano/node/`
- Holds refs to two nodes (source and destination), test-only utility
- Update to use new transport include paths

### Phase 6: Restructure transport_service

**6a. Update `transport_service`** to own the components:
- Creates `io_context` + `thread_runner` (already does this)
- Creates `transport_context` with injected deps
- Owns `tcp_service` (constructed with `transport_context&`)
- Owns `tcp_listener` (constructed with `transport_context&`)
- Provides `start()` / `stop()` for lifecycle

### Phase 7: Wire in node + update all includes

**7a. Have `nano::network` implement `handshake_provider`**
- Add `: public nano::transport::handshake_provider` to class declaration
- Existing methods already match the interface signatures

**7b. Create `transport_service` in node constructor**
- Build `bandwidth_limiter_config` from `node_config`/`node_flags`
- Create `bandwidth_limiter` owned by transport_service
- Construct `transport_context` with all deps + wire all callbacks
- Construct `transport_service` with context

**7c. Big-bang update all include paths**
- Replace all `#include <nano/node/transport/...>` with `#include <nano/transport/...>` across:
  - `nano/node/*.hpp/cpp` (~25 files)
  - `nano/node/bootstrap/*.hpp/cpp` (~6 files)
  - `nano/rpc/...` (if any)
  - Test files
- Replace all `nano/node/bandwidth_limiter.hpp` includes
- Update `nano/node/CMakeLists.txt`: remove transport files, add `nano_transport` to link deps
- Update `nano/transport/CMakeLists.txt`: add all moved files

**7d. Delete empty `nano/node/transport/` directory**
- Only `inproc.hpp/cpp` should remain (or it moves to a test utility location)

### Phase 8: Update `nano::network`

After transport extraction, `nano::network` becomes thinner:
- Instead of owning `tcp_channels` directly, it accesses `transport_service.tcp` (the `tcp_service`)
- Its peer selection methods (`list`, `random_set`, `flood_*`) call through to `tcp_service`
- Handshake methods stay on `network` (it implements `handshake_provider`)
- `syn_cookies` stays on `network`
- `network_filter` stays on `network` (transport gets a pointer to it)

## Critical files to modify

| File | Change |
|------|--------|
| `nano/transport/transport_service.hpp/cpp` | Restructure to own tcp_service + tcp_listener + transport_context |
| `nano/transport/transport_context.hpp` | **NEW** -- the central dependency container |
| `nano/transport/handshake_provider.hpp` | **NEW** -- abstract interface (3 virtual methods) |
| `nano/node/transport/channel.hpp/cpp` | `node&` -> `transport_context&`, move to `nano/transport/` |
| `nano/node/transport/tcp_socket.hpp/cpp` | `node&` -> `transport_context&`, move |
| `nano/node/transport/tcp_channel.hpp/cpp` | `node&` -> `transport_context&`, move |
| `nano/node/transport/tcp_server.hpp/cpp` | `node&` -> `transport_context&` + callbacks, move |
| `nano/node/transport/tcp_listener.hpp/cpp` | `node&` -> `transport_context&` + callbacks, move |
| `nano/node/transport/tcp_channels.hpp/cpp` | Rename to `tcp_service`, `node&` -> `transport_context&`, move |
| `nano/node/bandwidth_limiter.hpp/cpp` | Decouple config from node_config, move |
| `nano/node/network.hpp/cpp` | Implement `handshake_provider`, use `tcp_service` via transport_service |
| `nano/node/node.hpp/cpp` | Own `transport_service`, wire callbacks, create `transport_context` |
| `nano/node/CMakeLists.txt` | Remove transport files, link `nano_transport` |
| `nano/transport/CMakeLists.txt` | Add all moved files |
| `CMakeLists.txt` (root) | Ensure `nano/transport` before `nano/node` (already done) |

## Verification

1. **Build**: `.ai/ai_build.sh` -- must compile cleanly
2. **Tests**: `.ai/ai_test.sh --filter="network.*"` and `.ai/ai_test.sh --filter="tcp.*"` -- transport-related tests pass
3. **Full test suite**: `.ai/ai_test.sh` -- no regressions
4. **Include check**: `grep -r "nano/node/transport/" nano/transport/` returns nothing (transport lib doesn't include node)
5. **Dependency check**: `nano_transport` target does NOT link `node` target
