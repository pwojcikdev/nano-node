# RPC JSON Abstraction Design

## Problem

The RPC API must maintain backward compatibility with the ptree output format (all values as strings: `"123"`, `"true"`) because third-party integrations depend on it. At the same time, we want proper strict JSON (numbers as numbers, bools as bools) and decoupling from any specific JSON library.

## Current State

- **Legacy system** (`json_handler.cpp`, ~5000 lines): Uses `boost::property_tree::ptree`. All values are strings.
- **Modern system** (`nano/node/rpc_api/`): Uses `boost::json::object` directly. ~39 commands migrated.
- Commands manually stringify everything (`std::to_string()`, `"true"/"false"`) to match legacy output — defeats the purpose of boost::json.
- Dispatch layer (`inprocess_rpc_handler::process_request` in `json_handler.cpp` ~line 5219) tries modern registry first, falls back to legacy `json_handler`.

## Chosen Approach: Custom Wrapper Type

Own response types that capture semantic intent, convertible to any concrete JSON backend.

### Why not alternatives?

- **Approach B (Dual Serializer)**: Commands build `boost::json::object` with proper types, post-process to stringify. Simpler but still couples commands to boost::json. No backend independence.
- **SAX/streaming**: Overkill — RPC responses are small and bounded. Ledger I/O dominates, not serialization.
- **Policy-based template builder**: Adds template complexity for every contributor.

### Why this approach?

- Commands use our own types, not coupled to any JSON library
- Value variant captures semantic type (string/int/bool/etc.)
- Single conversion point at dispatch boundary
- Easy to add ptree, custom writer, or any future backend
- Implicit constructors make API as clean as `boost::json::object`

## Core Types (`response.hpp`)

```cpp
namespace nano::rpc_api
{
class response_object;
class response_array;

class response_value
{
public:
    // Implicit constructors for clean API
    response_value (std::string s);
    response_value (std::string_view s);
    response_value (char const * s);
    response_value (int64_t n);
    response_value (uint64_t n);
    response_value (bool b);
    response_value (response_object obj);
    response_value (response_array arr);
    static response_value null ();

    // Move-only
    response_value (response_value &&) = default;
    response_value & operator= (response_value &&) = default;

    enum class kind { string, int64, uint64, boolean, null, object, array };
    kind type () const;

    // Accessors
    std::string const & get_string () const;
    int64_t get_int64 () const;
    uint64_t get_uint64 () const;
    bool get_bool () const;
    response_object const & get_object () const;
    response_array const & get_array () const;

private:
    using variant_t = std::variant<
        std::string, int64_t, uint64_t, bool, std::nullptr_t,
        std::unique_ptr<response_object>,
        std::unique_ptr<response_array>>;
    variant_t data_;
};

class response_object
{
public:
    void set (std::string key, response_value val);
    using entry = std::pair<std::string, response_value>;
    std::vector<entry> const & entries () const;
    bool empty () const;
private:
    std::vector<entry> fields_;  // preserves insertion order
};

class response_array
{
public:
    void push_back (response_value val);
    std::vector<response_value> const & elements () const;
    bool empty () const;
    std::size_t size () const;
private:
    std::vector<response_value> elements_;
};
}
```

## Converters (`response_json.hpp`)

```cpp
namespace nano::rpc_api
{
enum class json_format
{
    compat, // All leaf values as strings (backward-compatible with ptree)
    strict  // Proper JSON types
};

boost::json::value to_json (response_value const & val, json_format format);
boost::json::object to_json (response_object const & obj, json_format format);
boost::json::array to_json (response_array const & arr, json_format format);
std::string serialize (response_object const & obj, json_format format);
}
```

### Compat mode conversion rules

| Wrapper type | compat output | strict output |
|---|---|---|
| `string` | string (unchanged) | string |
| `int64` | `std::to_string(n)` | number |
| `uint64` | `std::to_string(n)` | number |
| `bool` | `"true"` / `"false"` | `true` / `false` |
| `null` | `""` | `null` |
| `object/array` | recurse | recurse |

### "1"/"0" vs "true"/"false" booleans

Legacy API uses both patterns. Handle naturally:
- `response.set("confirmed", true)` → compat: `"true"`, strict: `true`
- `response.set("started", 1)` → compat: `"1"`, strict: `1`

Command author picks `bool` or `int` based on what the legacy API produced.

### uint128_t (balance, amount, weight)

Too large for JSON number types. Always pass as `.convert_to<std::string>()`. Stays string in both compat and strict modes. The wrapper doesn't need to know about uint128_t specifically.

## Command Usage Pattern

```cpp
// Before (boost::json coupled):
boost::json::object response;
response["balance"] = balance.convert_to<std::string> ();
response["count"] = std::to_string (info->block_count);
response["confirmed"] = confirmed ? "true" : "false";
return command_result::ok (response);

// After (wrapper, proper types):
response_object response;
response.set ("balance", balance.convert_to<std::string> ()); // uint128 → stays string
response.set ("count", info->block_count);                    // uint64_t → numeric
response.set ("confirmed", confirmed);                        // bool → boolean
return command_result::ok (std::move (response));

// Arrays:
response_array accounts;
for (auto const & acct : wallet->accounts ())
    accounts.push_back (acct.to_account ());
response.set ("accounts", std::move (accounts));

// Nested objects:
response_object blocks;
for (auto const & [hash, block] : results) {
    response_object entry;
    entry.set ("balance", block.balance ().number ().convert_to<std::string> ());
    entry.set ("height", block.sideband ().height);
    blocks.set (hash.to_string (), std::move (entry));
}
response.set ("blocks", std::move (blocks));
```

## Changes to command.hpp

Replace `boost::json::object` with `response_object` in `command_result` and `request_context::respond`. Request parsing stays as `boost::json` (params.hpp abstraction is already good).

```cpp
class command_result {
    static command_result ok (response_object value);
    // ...
    std::optional<response_object> value_;
};

struct request_context {
    std::function<void (response_object)> respond;     // was boost::json::object
    boost::json::object const & params () const;        // request parsing unchanged
};
```

## Integration at dispatch layer

`json_handler.cpp` (~line 5244):
```cpp
[response_a] (nano::rpc_api::response_object result) {
    response_a (nano::rpc_api::serialize (result, nano::rpc_api::json_format::compat));
}
```

Format can later be configurable per-request (`"strict_json": true`) or via `node_rpc_config`.

## Future backend extensibility

```cpp
// ptree converter (if needed for legacy bridge)
boost::property_tree::ptree to_ptree (response_object const & obj);

// Direct-to-string writer (skip intermediate boost::json allocation)
std::string serialize_fast (response_object const & obj, json_format format);
```

## File manifest

| File | Status |
|---|---|
| `nano/node/rpc_api/response.hpp` | NEW — wrapper types |
| `nano/node/rpc_api/response.cpp` | NEW — constructors, accessors |
| `nano/node/rpc_api/response_json.hpp` | NEW — boost::json converter declarations |
| `nano/node/rpc_api/response_json.cpp` | NEW — converter implementations |
| `nano/node/rpc_api/command.hpp` | MODIFY — response_object in command_result, request_context |
| `nano/node/json_handler.cpp` | MODIFY — respond lambda uses serialize(result, format) |
| `nano/node/CMakeLists.txt` | MODIFY — add 4 new files |
| `nano/node/rpc_api/commands/*.cpp` (23 files) | MIGRATE — boost::json::object → response_object |

## Rollout phases

**Phase 1 — Infrastructure:** Add wrapper types, converter, wire into dispatch with `compat` hardcoded. No behavior change. Add unit tests.

**Phase 2 — Command migration:** Convert commands to `response_object`. Replace `std::to_string()` with raw numeric, `"true"/"false"` with raw bool. Existing tests pass unchanged (compat mode stringifies back).

**Phase 3 — Configuration:** Add format to `node_rpc_config`, per-request override, strict-mode tests.

## Commands that need special attention during migration

- `block_info`, `block_hash`, `block_create` — bridge ptree for block serialization (`serialize_json` returns ptree)
- `telemetry`, `stats` — use `ptree_to_json()` helper to convert legacy subsystem output
- Async commands (`work_set`, `stop`) — call `ctx.respond()` directly, format applied in lambda
- `account_history` — stubbed, depends on decoupling `history_visitor` from ptree
