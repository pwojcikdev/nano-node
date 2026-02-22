# Plan: Migrate Blocks from shared_ptr Polymorphism to Value Types

## Context

The current block system uses `std::shared_ptr<nano::block>` with virtual inheritance (5 concrete block types inheriting from abstract `nano::block`). Sideband is optional, leading to bugs. This refactoring replaces it with:
- **`raw_block`** (already exists): variant-based, contains only transmitted data (hashables, signature, work). No sideband.
- **`stored_block`** (new): value type wrapping `raw_block` + `block_sideband`. Sideband is **required**. Will be renamed to `block` at the end.

The `raw_block` infrastructure and conversion bridges (`to_raw()`/`to_legacy()`) are already in place on this branch.

---

## Phase 1: Create `stored_block` + Conversion-Based Compatibility (Non-Breaking)

**Goal**: Add the new value type with conversion operators so it can be used in place of the old type. Change `ledger_set_any::block_get()` to return `std::optional<stored_block>`. Existing code keeps working via conversions.

### 1a. Create `nano::stored_block` class

**New file**: `nano/lib/stored_block.hpp` / `.cpp`

```cpp
class stored_block {
    raw_block raw_m;
    block_sideband sideband_m;
public:
    stored_block(raw_block raw, block_sideband sideband);

    // Construct from legacy block (extracts raw + sideband)
    explicit stored_block(nano::block const & legacy);

    // Conversion to legacy shared_ptr<block> (for backward compat)
    std::shared_ptr<nano::block> to_legacy() const;
    operator std::shared_ptr<nano::block>() const;  // implicit conversion

    // Delegated to raw_block
    block_hash const & hash() const;
    block_type type() const;
    signature const & block_signature() const;
    root root() const;
    uint64_t block_work() const;
    qualified_root qualified_root() const;
    block_hash previous() const;

    // Field accessors (from raw_block, return optional)
    std::optional<account> account_field() const;
    std::optional<amount> balance_field() const;
    // ... (all 7 field accessors)

    // Canonical accessors (resolve via sideband when field missing)
    account account() const noexcept;
    amount balance() const noexcept;

    // Sideband
    block_sideband const & sideband() const;

    // Sideband-derived
    bool is_send() const noexcept;
    bool is_receive() const noexcept;

    // Access underlying raw_block
    raw_block const & raw() const;
    raw_block_variant const & variant() const;

    // Pointer-like access for compatibility (block_get returns optional<stored_block>)
    stored_block const * operator->() const { return this; }

    bool operator==(stored_block const &) const;
};
```

### 1b. Add mutable variant access to `raw_block`

Add `raw_block_variant & variant_mut()` — needed for `ledger_processor`.

### 1c. Change `block_get` to return `std::optional<stored_block>`

Change `ledger_set_any::block_get()` return type from `shared_ptr<block>` to `std::optional<stored_block>`. Existing callers that do null checks (`if (block)`) work with `std::optional` already. Callers that use `->` work because `stored_block` provides `operator->`. Callers that need `shared_ptr<block>` use the implicit conversion operator.

### 1d. Add `ledger.process(tx, raw_block)` overload

Keep existing `process(tx, shared_ptr<block>)`. Add new overload that does the real work. Old overload converts via `to_raw()` and delegates.

**Files**: `nano/lib/stored_block.hpp`, `nano/lib/stored_block.cpp`, `nano/lib/blocks_raw.hpp`, `nano/secure/ledger.hpp`, `nano/secure/ledger_set_any.hpp`, `nano/store/ledger/block.hpp/.cpp`

---

## Phase 2: Rewrite Ledger Processor Using raw_block (Internal)

**Goal**: Core processing path uses `raw_block` + `std::visit` instead of visitor pattern.

### Changes:
- Rewrite `ledger_processor` to accept `raw_block`, use `std::visit` with `if constexpr` instead of inheriting `mutable_block_visitor`
- Sideband is created locally and stored alongside the raw block
- Store gets new overload: `block_view::put(tx, hash, raw_block, block_sideband)`
- Rewrite `ledger_rollback` similarly
- Rewrite `representative_block_visitor` (in ledger.cpp) as free function

**Files**: `nano/secure/ledger_processor.hpp/.cpp`, `nano/secure/ledger_rollback.hpp/.cpp`, `nano/secure/ledger.cpp`, `nano/store/ledger/block.hpp/.cpp`

---

## Phase 3: Migrate Store Layer

**Goal**: Store uses raw_block + sideband internally.

### Changes:
- Update `block_w_sideband` to use `raw_block` instead of `shared_ptr<block>`
- Update `db_val` conversions (`nano/store/db_val_templ.hpp`) to deserialize into `raw_block` + `block_sideband`
- Update `block_view::get()` to return `std::optional<stored_block>`
- Keep old `shared_ptr<block>` returning method as wrapper using `to_legacy()`
- Remove `block_uniquer` dependency from deserialization (value types don't need dedup)

**Files**: `nano/store/block_w_sideband.hpp`, `nano/store/db_val_templ.hpp`, `nano/store/db_val.hpp`, `nano/store/ledger/block.hpp/.cpp`

---

## Phase 4: Migrate block_processor Queue

**Goal**: Block processor uses `raw_block` instead of `shared_ptr<block>`.

### Changes:
- Update `block_context` (`nano/node/block_context.hpp`) to hold `raw_block`
- Update `block_processor::add()` to accept `raw_block` (keep old overload as wrapper)
- Update `unchecked_info` to hold `raw_block`

**Files**: `nano/node/block_context.hpp`, `nano/node/block_processor.hpp/.cpp`

---

## Phase 5: Migrate Elections

**Goal**: Elections use `raw_block` for fork tracking, started with `stored_block`.

### Design:
- Election is **started** with a `stored_block` (confirmed in ledger, has sideband)
- Competing forks arrive as `raw_block` (from network, no sideband)
- Election internally stores `raw_block` for all candidates
- When sideband info is needed, look up from ledger

### Changes:
- Update `election` to store `raw_block` for block candidates
- Update `election::publish()` to accept `raw_block`
- Election start requires a `stored_block` (extracts raw internally)
- Update `fork_cache` to store `raw_block`
- Update `election_status` to reference blocks by hash or hold `raw_block`
- Update `tally_t` map value type

**Files**: `nano/node/election.hpp/.cpp`, `nano/node/active_elections.hpp/.cpp`, `nano/node/fork_cache.hpp/.cpp`

---

## Phase 6: Migrate Network Layer

**Goal**: Network messages use `raw_block`.

### Changes:
- Update `publish` message to hold `raw_block` instead of `shared_ptr<block>`
- Update `block_deserializer` callback to return `raw_block`
- Update `network_filter` to work with `raw_block`

**Files**: `nano/node/messages.hpp/.cpp`, `nano/node/block_deserializer.hpp/.cpp`, `nano/node/network_filter.hpp/.cpp`

---

## Phase 7: Migrate Tests (Bulk)

**Goal**: All ~550+ `.build()` calls migrated to `.build_raw()`.

### Test Helper (add to `nano/test_common/`):
```cpp
namespace nano::test {
nano::stored_block process_and_get(nano::ledger &, secure::write_transaction const &, nano::raw_block);
}
```

### Mechanical Transformation Rules:

| Old Pattern | New Pattern |
|---|---|
| `builder...build()` | `builder...build_raw()` |
| `b->hash()` | `b.hash()` |
| `b->type()` | `b.type()` |
| `b->sideband().height` | `stored.sideband().height` (get from ledger) |
| `b->account()` | `stored.account()` (get from ledger) |
| `ledger.process(tx, b)` | `ledger.process(tx, b)` (overloaded) |
| `ASSERT_NE(nullptr, b)` | `ASSERT_TRUE(b.has_value())` |
| `ASSERT_EQ(*a, *b)` | `ASSERT_EQ(a, b)` |
| `node.process_active(b)` | `node.process_active(b)` (overloaded) |

### Key semantic shift:
- **Sideband is accessed on the stored/retrieved block**, not on the locally-built block (because `raw_block` has no sideband)
- Tests that check sideband after `process()` must first retrieve the block from the ledger

### Migration order (by impact):
1. `core_test/ledger.cpp` (316 build calls) — validates the approach
2. `core_test/node.cpp` (147 calls)
3. `rpc_test/rpc.cpp` (102 calls)
4. `core_test/ledger_cement.cpp` (58 calls)
5. `core_test/active_elections.cpp` (56 calls)
6. `test_common/` helpers (chains.cpp, ledger_context.cpp, etc.)
7. Remaining ~45 files (< 20 calls each)

---

## Phase 8: Rename and Cleanup

**Goal**: Remove all legacy types.

### Changes:
- Remove polymorphic `nano::block` hierarchy (send_block, receive_block, etc. as classes)
- Rename `stored_block` → `block`
- Remove `block_visitor` and `mutable_block_visitor`
- Remove `to_raw()` / `to_legacy()` conversion bridges
- Remove `block_uniquer` (no dedup needed for value types)
- Remove `optional_ptr<block_sideband>` from old block class
- Remove `block_builder::build()` (keep only `build_raw()`)
- Remove custom memory pool allocators for blocks

---

## Verification

Each phase must:
1. **Compile**: `.ai/ai_build.sh`
2. **Pass all tests**: `.ai/ai_test.sh`
3. **Format**: `.ai/ai_format.sh --check`

Key test suites to verify:
- `--filter="ledger.*"` — core ledger operations
- `--filter="block.*"` — block-specific tests
- `--filter="node.*"` — integration tests
- Full test suite after each phase
