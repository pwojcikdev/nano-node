# Plan: Integration Tests for v3 Handshake + Channel Capability Filtering

## Context

The v3 handshake has been implemented (serialization, signature, channel storage) but:
1. No integration tests verify that flags are exchanged when live nodes connect
2. `prepare_handshake_response` hardcodes `flags = none` — need to plumb real capabilities
3. No way to filter/query channels by their advertised capabilities

## Files to Modify

### 1. `nano/node/network.hpp` — Add `capabilities` member

Add a public `capabilities` field to `nano::network` (near line 189, in the public members section):
```cpp
nano::node_capabilities_flags capabilities;
```
Defaults to `none`. Tests set it before connecting: `node->network.capabilities.set(nano::node_capabilities::topo_index)`.

### 2. `nano/node/network.cpp` — Read capabilities in handshake response

In `prepare_handshake_response` (line ~692), replace hardcoded none with `capabilities`:
```cpp
v3.flags = capabilities;  // was: default-constructed (none)
```
Remove the TODO comment.

### 3. `nano/node/transport/tcp_channels.hpp` — Add capability-filtered `list` overload

Add a new overload alongside existing `list` methods (line ~56):
```cpp
std::deque<std::shared_ptr<nano::transport::channel>> list (nano::node_capabilities_flags required) const;
```
Returns channels that have **all** the specified capability flags set.

### 4. `nano/node/transport/tcp_channels.cpp` — Implement capability-filtered `list`

```cpp
std::deque<std::shared_ptr<nano::transport::channel>> nano::transport::tcp_channels::list (nano::node_capabilities_flags required) const
{
    nano::lock_guard<nano::mutex> lock{ mutex };
    std::deque<std::shared_ptr<nano::transport::channel>> result;
    for (auto const & entry : channels)
    {
        if ((entry.channel->get_flags () & required) == required)
        {
            result.push_back (entry.channel);
        }
    }
    return result;
}
```

### 5. `nano/core_test/tcp_server.cpp` — Integration tests

**Test 1: `tcp_server.handshake_v3_flags_exchanged`**
- Create two nodes
- Set `node1->network.capabilities.set(topo_index)`, `node2->network.capabilities.set(vote_storage)`
- Connect via `merge_peer`
- Wait for channels (`find_node_id`)
- Verify: node1's channel to node2 has `vote_storage` (received from node2)
- Verify: node2's channel to node1 has `topo_index` (received from node1)
- No handshake aborts

**Test 2: `tcp_server.handshake_v3_no_flags`**
- Two nodes with default capabilities, connect, verify `flags.none()` on both channels

**Test 3: `tcp_server.handshake_v3_filter_by_capability`**
- Create 3 nodes: node1 has `topo_index`, node2 has `vote_storage`, node3 has both
- Connect all to each other
- On node1, `tcp_channels.list(vote_storage)` should return channels to node2 and node3
- On node1, `tcp_channels.list(topo_index)` should return channel to node3 only
- On node1, `tcp_channels.list(topo_index | vote_storage)` should return channel to node3 only

## Key code paths

- `network::prepare_handshake_response()` (`nano/node/network.cpp:681`) — creates v3_payload, reads `this->capabilities`
- `tcp_server::process_handshake()` (`nano/node/transport/tcp_server.cpp:304`) — extracts `message.response->flags()`
- `tcp_server::to_realtime_connection()` (`nano/node/transport/tcp_server.cpp:496`) — passes flags to `tcp_channels::create()`
- `tcp_channels::create()` (`nano/node/transport/tcp_channels.cpp:89`) — calls `channel->set_flags(flags)`
- `channel::get_flags()` (`nano/node/transport/channel.hpp:112`) — returns stored flags
- `tcp_channels::list(channel_filter)` (`nano/node/transport/tcp_channels.cpp:431`) — existing generic filter pattern

## Verification

1. Build: `.ai/ai_build.sh | tail -10`
2. Run integration tests: `.ai/ai_test.sh --filter="tcp_server.handshake*"`
