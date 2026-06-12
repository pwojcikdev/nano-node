# Frontier Scan: Heads + Rounds on a Threaded Driver — Final Design

## Decisions

- **A (state ownership):** Persistent `head_state` = {range, cursor, `pause_until`, lifetime counters, `unique_ptr<round_state>`}. Transient first-class `round_state` = {position, pure `frontier_round` votes, `used` peers, `tag_ids`, `launched`, `last_launch`}; constructed at **first commit**, destroyed **only** in `conclude ()`. Engine = head table + round-robin cursor. Strategy = thread + 1-thread classification pool + probe closures.
- **B (in-flight):** Derived from `ctx.tags` via the round's **recorded tag ids** (`probes.count_inflight (span<id_t>)`), not a round-internal counter and not position-keyed `count_tags`. Two sanctioned lifecycle fixes: `transmit ()` erases the tag on synchronous send failure; the maintenance sweep erases **all** expired tags (full scan), not just the expired prefix.
- **C (engine API):** Engine owns iteration: `settle (now, probes)` sweep (always runs, never gated) + `next_launch (now, probes)` slot picker. The single settlement point is the private `conclude (head, now)`, called only from `settle ()` on the driver thread. `commit ()` runs **before** the send, in the **same ctx.mutex critical section** as probe → limiter → acquire (no settled-recheck needed; nothing can interleave).
- **D (residence):** Engine is the ctx shared-state member `ctx.frontiers` (type swap of `frontier_scan_index` → `frontier_scan_engine`); `ctx.reset ()` and `container_info ()` wiring unchanged; sibling/topo strategies follow the same slot pattern.
- **E (driver loop):** Predicate order under one lock: settle → backpressure gates (accounts, classification queue, global tags cap) → `next_launch` with a **capacity-aware** peer probe → limiter token → acquire (guaranteed modulo a documented send-queue residual) → commit. Send off-lock; `erase_sample` on sync send failure; park on `ctx.wait` capped backoff (≤100 ms).
- **F (tests):** Ported `frontier_round` suite verbatim + 18 deterministic engine tests with injected clock, fake tri-state peer probe, fake live-tag set (enumerated below); system tests `bootstrap.frontier_scan*` unchanged.
- **G (topo):** Identical four-layer shape (spear + repair heads, `page_round` voting core, tag-id-bound rounds, tags-derived in-flight, capability flag baked into the probe closure) plus one structural addition committed now: settlement outputs flow through an **engine-owned outbox** drained by the driver and submitted off-lock.

## Summary

Replace `frontier_scan_index` (lifecycle smeared across `next ()`/`process ()`) with a `frontier_scan_engine` owning persistent **heads** and first-class transient **rounds**, living exactly where the index lives today (`ctx.frontiers`, caller-synchronized under `ctx.mutex`). The pure voting core `frontier_round` and `classify_frontiers` are ported verbatim from `bootstrap-coro:nano/node/bootstrap/frontier_scan.{hpp,cpp}` (settled). The strategy keeps its single driver thread and 1-thread classification pool.

The backbone is the coro-fidelity design — it preserves every proven coro semantic (single settlement point, probe-before-budget starvation fix, straggler rejection, non-clean cooldown pacing, empty-responses-feed fix, stragglers-still-classify) with exact round identity via recorded tag ids. Onto it, this final design grafts the fixes demanded by the adversarial critiques of all three designs:

1. **Capacity-aware launch probe** (`peer_pool::probe` tri-state, mirrors `acquire` without reserving). `has_candidate` is capacity-blind (verified: `peer_pool.cpp:78-84` checks only capability + exclusion), so all three designs burned 15/s limiter tokens on busy pools, amplified by the shared condvar's notify storms (`bootstrap_context.cpp:521` notifies on every response of every strategy). With the tri-state probe, a token is consumed only when acquire is guaranteed to succeed within the same critical section — the busy starvation/burn class is closed, not bounded.
2. **No global pauses, no fused busy-park:** `next_launch` *skips* a busy head and continues the rotation in the same pass, so one pinned head can never starve a launchable one (the pure-engine livelock and the pragmatic 1 s head-of-line park are both structurally impossible).
3. **Cheap-first predicate costs:** `settled ()` (O(1)) → `count_inflight` (≤16 hash lookups) → peer probe last, only when in-flight is zero and the pacing window is open; one memoized empty-exclude probe per pass for round-less heads. Quantified residual bound below.
4. **Validated mutators:** `erase_sample`/`process` are no-ops (+stat) when the round is gone or doesn't own the id — the reset-interleaving underflow/UAF class (flagged as major against both alternative designs) cannot occur.
5. **Complete maintenance sweep:** the front-pop loop (`bootstrap_context.cpp:432-439`) becomes a full scan, removing the verified head-of-line blocking where one unconfirmed-send tag (cutoff `request_timeout * 4`) delays erasure of every younger expired tag by up to ~45 s.
6. **Topo-proof settlement outputs:** engine settlement writes to an internal outbox the driver drains off-lock — empty/absent for frontiers, page retirement for topo — so the topo port is a parameterization, not an interface break.

Two deliberate semantic carries from coro that fix bugs in the current branch: empty responses **feed the round** (today `frontier_strategy.cpp:113-117` early-returns, making `done_empty`/range-wrap unreachable), and straggler responses **still classify** (frontier data is ledger truth regardless of round bookkeeping).

Files: new `nano/node/bootstrap/frontier_scan.{hpp,cpp}` (ported core) and `nano/node/bootstrap/frontier_scan_engine.{hpp,cpp}` (replaces `frontier_scan_index.{hpp,cpp}`); rewritten `nano/node/bootstrap/frontier_strategy.{hpp,cpp}`; edits to `nano/node/bootstrap/bootstrap_context.{hpp,cpp}` (member type swap, `send` id-overload, transmit fix, sweep fix), `nano/node/bootstrap/peer_pool.{hpp,cpp}` (`probe`), `nano/node/bootstrap/queries.cpp` (frontiers `index_keys` → `{start, 0}`, diagnostics only), `nano/lib/stats_enums.hpp` (`done_partial`, `done_empty_partial`, `done_none`); rewritten `nano/core_test/bootstrap_frontier_scan.cpp`.

## State ownership

| State | Owner | Resets when |
|---|---|---|
| range `[start, end)`, `cursor`, `pause_until`, `rounds`, `processed` | **head** (persistent) | `reset ()` only; cursor moves only in `conclude ()` |
| `frontier_round` votes, `used` node ids, `tag_ids`, `launched`, `last_launch` | **round** (transient, first-class) | wholesale, by destroying the object in `conclude ()` — never by clearing fields on the head |
| head table, round-robin index `robin` | **engine** (ctx-owned) | `reset ()` |
| in-flight sample count | **ctx.tags** (derived via the round's `tag_ids`) | automatically, on every tag-erasure path |
| channels, limiter, classification queue, send | **strategy** (stateless I/O shell) | — |

The coro `round_state`'s `inflight` counter and `open` flag are *replaced*, not flattened: in-flight is derived, and "open" is object existence — a destroyed round can by construction neither be fed nor launched into. `launched`/`last_launch` were round-loop locals in coro and stay **in the round** (flattening exactly these onto the head was the rejected previous attempt).

```cpp
// nano/node/bootstrap/frontier_scan_engine.hpp  (replaces frontier_scan_index.hpp)
#pragma once

#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/frontier_scan.hpp> // frontier_round + classify_frontiers, ported from bootstrap-coro
#include <nano/node/bootstrap/peer_pool.hpp> // probe_status

namespace nano::bootstrap
{
/*
 * Heads + rounds orchestration for the frontier scan. The account space is divided into equal
 * ranges, each owned by a persistent head advancing a cursor. One round samples the frontiers at
 * the cursor from several distinct peers, votes, and advances at a single settlement point.
 *
 * Pure bookkeeping: no I/O, time is injected, the outside world is observed through `probes`.
 * Not internally synchronized: callers hold the bootstrap mutex.
 *
 * Threading contract: settle (), next_launch (), commit () and erase_sample () may only be called
 * by the single driver thread; process () and reset () additionally from the dispatch/maintenance
 * paths under the same mutex. The driver's commit () -> transmit -> (erase_sample on failure)
 * sequence is one uninterrupted driver turn: no settle () can observe a committed id that is not
 * yet in the tags table, because only the driver settles. Tests must mirror this ordering.
 */
class frontier_scan_engine
{
public:
	// How the engine observes the world; faked in tests
	struct probes
	{
		// Capacity-aware peer availability for the exclusion list (peer_pool::probe):
		// available = acquire would succeed; busy = candidates exist but are at capacity;
		// none = every capable peer is excluded or the pool is empty
		std::function<peer_probe_status (std::span<nano::account const> exclude)> peer_status;
		// How many of the given requests are still tracked (ids present in ctx.tags)
		std::function<size_t (std::span<id_t const> tag_ids)> count_inflight;
	};

	struct launch_slot
	{
		size_t head_index;
		nano::account position;
		std::span<nano::account const> exclude; // Aliases round.used: valid until the next mutating engine call
	};

	frontier_scan_engine (nano::frontier_scan_config const &, nano::stats &);

	// Settlement sweep: concludes every settle-ripe round (quorum, exhausted-with-nothing-in-flight,
	// or sample-cap reached). Always run first in the driver predicate, never gated, so settlement
	// latency is bounded by the poll cap even when launches are blocked. Cheap-first internally.
	void settle (std::chrono::steady_clock::time_point now, probes const &);

	// Round-robin scan for a head that may launch a sample now. Surfaces a slot only when
	// probes.peer_status reports `available`, so a subsequent acquire under the same mutex hold
	// succeeds (modulo the send-queue residual, see driver). Busy heads are skipped within the
	// same pass; the walk covers one full rotation. Does not create or mutate rounds.
	std::optional<launch_slot> next_launch (std::chrono::steady_clock::time_point now, probes const &);

	// Commits one launched sample: creates the round at the head's cursor if absent, records the
	// distinct peer, the tag id and the launch time. Must precede the actual send.
	// `position` must equal the head cursor / round position (asserted).
	void commit (size_t head_index, nano::account const & position, nano::account const & node_id, id_t tag_id, std::chrono::steady_clock::time_point now);

	// Forgets a sample whose send failed synchronously. The peer stays in `used` (implicit penalty
	// until pool decay) and `launched`/`last_launch` pacing is kept (a broken peer must not be
	// machine-gunned). No-op (+ stat) when the head has no round or the round does not own the id
	// (legal after a concurrent reset ()).
	void erase_sample (size_t head_index, id_t tag_id);

	// Feeds one verified sample to the round that launched the tag. Returns false when no open
	// round owns the tag (straggler from a settled round, or post-reset); feeds nothing then.
	// Empty frontier lists are valid samples and count toward the empty-range consensus.
	bool process (id_t tag_id, nano::account const & start, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	void reset (); // Drops all rounds (their in-flight requests become stragglers) and rewinds cursors

	nano::container_info container_info () const; // Per-head progress / rounds / processed + open rounds

	// A round stops launching and becomes conclude-ripe (once nothing is in flight) after this many
	// samples, bounding `used` growth and round lifetime under peer churn with universally lost responses
	static size_t constexpr max_round_samples_factor = 4; // cap = consideration_count * factor

private: // Dependencies
	nano::frontier_scan_config const & config;
	nano::stats & stats;

private:
	// Transient: ONE fanout round at a fixed position. Everything that resets at settlement lives
	// here and resets by object destruction in conclude (), the single settlement point.
	struct round_state
	{
		nano::account const position; // Fixed for the round's lifetime; equals the query start of every sample
		frontier_round votes; // Pure voting core: feed/done/empty_range/settled/conclude (ported)
		std::vector<nano::account> used; // Node ids sampled this round; enforces the distinct-peer rule
		std::vector<id_t> tag_ids; // Requests launched by this round; binds responses and derives in-flight
		size_t launched{ 0 };
		std::chrono::steady_clock::time_point last_launch{};

		round_state (nano::frontier_scan_config const & config, nano::account position_a, nano::account range_end);
		bool owns (id_t tag_id) const;
	};

	// Persistent: range, cursor, lifetime counters, inter-round pacing
	struct head_state
	{
		size_t const index;
		nano::account const start;
		nano::account const end; // Scanned range is [start, end)
		nano::account cursor; // Only conclude () may move, wrap, or (by leaving it) retry this
		std::unique_ptr<round_state> round; // Engaged while a round is in progress; null between rounds
		std::chrono::steady_clock::time_point pause_until{}; // Cooldown pacing after non-clean conclusions
		size_t rounds{ 0 };
		size_t processed{ 0 };
	};

	void conclude (head_state &, std::chrono::steady_clock::time_point now); // The ONLY mutator of cursor/pacing/round lifetime
	head_state & find_head (nano::account const & position); // Floor lookup over the fixed sorted range starts

	std::vector<head_state> heads; // Built once in the ctor (reserve first; const members forbid reassignment)
	size_t robin{ 0 }; // Fairness cursor, advanced when next_launch returns a slot
};
}
```

Invariants worth stating in code comments: **round exists ⟹ launched ≥ 1** (round created at first commit — the threaded equivalent of coro's `launched == 0` wait-for-peers branch is simply "no round, probe-gated"); **cursors never regress** except by explicit range wrap (`conclude ()` asserts `target > cursor`, faithful to `frontier_round::conclude`'s `> position` candidate filter).

## API

### peer_pool addition

```cpp
// peer_pool.hpp
enum class peer_probe_status
{
	available, // acquire () would reserve a peer right now
	busy, // Capable non-excluded peers exist but all are at capacity; wait for capacity
	none, // No capable non-excluded peer (pool empty or all excluded); conclude the work
};

// Capacity-aware, non-reserving mirror of acquire (): capability + exclusion + outstanding < channel_limit.
// Deliberately does NOT check channel->max (traffic) (that requires locking the channel; acquire checks it
// on the best candidate only) — so `available` can rarely be answered by a busy acquire when every
// below-capacity candidate's send queue is full. Callers treat that as a bounded, stat-counted no-launch.
peer_probe_status probe (nano::node_capabilities_flags required = {}, std::span<nano::account const> exclude = {}) const;
```

`probe (...) == none` is exactly `!has_candidate (...)`; `has_candidate` is retained for its existing callers/tests but the engine consumes only `probe`.

### bootstrap_context changes

```cpp
// bootstrap_context.hpp
nano::bootstrap::frontier_scan_engine frontiers; // Type swap; reset () and container_info () wiring unchanged

// New overload: caller-supplied id, so the request is bound to its round BEFORE transmission
bool send (std::shared_ptr<nano::transport::channel> const &, query_descriptor query, query_source source, id_t id);
// Existing 3-arg send delegates with generate_id ()
```

```cpp
// bootstrap_context.cpp :: transmit — targeted improvement (sanctioned by constraint 4)
if (!sent)
{
	// channel->send returned false synchronously: the completion callback provably never fires, so
	// the tag would linger as a phantom until the timeout sweep (cutoff request_timeout * 4),
	// inflating tag-derived in-flight counts. Erase it now. Idempotent vs the callback: it finds
	// nothing and no-ops.
	nano::lock_guard<nano::mutex> lock{ mutex };
	tags.get<tag_id> ().erase (tag.id);
}
```

```cpp
// bootstrap_context.cpp :: maintenance — targeted improvement: complete timeout sweep.
// The old front-pop loop stopped at the first non-expired tag; cutoffs are non-monotonic in
// insertion order (send-success shortens cutoff from insert+4T to send+T), so one
// unconfirmed-send tag could block erasure of every younger expired tag for up to ~45s,
// stalling tag-derived in-flight decrements across all strategies. Full scan is O(max_requests=1000)
// per tick (500ms dev / 5s prod) under the already-held lock — cheap.
auto & tags_by_order = tags.get<tag_sequenced> ();
for (auto it = tags_by_order.begin (); it != tags_by_order.end ();)
{
	if (should_timeout (*it))
	{
		stats.inc (nano::stat::type::bootstrap, nano::stat::detail::timeout);
		stats.inc (nano::stat::type::bootstrap_timeout, to_stat_detail (it->type ()));
		it = tags_by_order.erase (it);
	}
	else
	{
		++it;
	}
}
```

```cpp
// queries.cpp :: index_keys — frontiers tags keyed by position. Diagnostics only: the engine binds
// by tag id, but this makes count_tags/introspection meaningful for frontiers and establishes the
// keying pattern the topo strategy will reuse (topo pages keyed via the hash index).
query_keys operator() (frontiers_query const & query) const
{
	return { query.start, nano::block_hash{ 0 } };
}
```

### Strategy

```cpp
// frontier_strategy.hpp
class frontier_strategy
{
public:
	explicit frontier_strategy (bootstrap_context & ctx);
	void start ();
	void stop ();
	void run_one ();

	bool process (nano::messages::asc_pull_ack::frontiers_payload const & response, async_tag const & tag);

private:
	void run ();
	void classify (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers); // Worker task: ported classify_frontiers + priority_set

	bootstrap_context & ctx;
	frontier_scan_engine::probes probes; // Constructed once in the ctor (closures capture only `this`); avoids per-poll std::function churn
	std::thread thread;
	nano::thread_pool workers; // 1 thread, ledger classification (unchanged)
};
```

```cpp
// Probe wiring (ctor); call sites hold ctx.mutex by construction
probes{
	.peer_status = [this] (std::span<nano::account const> exclude) {
		return ctx.peers.probe ({}, exclude);
	},
	.count_inflight = [this] (std::span<id_t const> ids) {
		auto const & by_id = ctx.tags.get<bootstrap_context::tag_id> ();
		return static_cast<size_t> (std::count_if (ids.begin (), ids.end (), [&] (id_t id) { return by_id.contains (id); }));
	},
}
```

### Why decision B is tag-id sets (the decisive arguments)

- **vs a round-internal counter:** every decrement path must be hand-routed — maintenance sweep, async send-error callback, sync send failure, *and* the dispatch path that erases the tag but rejects the payload type before the strategy is ever called (`invalid_response_type`, `bootstrap_context.cpp:497-502`, verified). That last one permanently strands a counter: a peer echoing our id with a blocks payload would block `exhausted ⇒ conclude` (which requires in-flight == 0) forever on a small pool. Deriving from the tags table makes every erasure path — present and future — an automatic decrement.
- **vs position-keyed `count_tags`:** position conflates rounds. After a nothing-learned retry at the same position, the predecessor's still-live tags would (a) delay the new round's exhausted-conclude and (b) require a separate correlation mechanism for feeds anyway (a `frontiers_query::round` field, as the pragmatic design needed). Tag-id sets give exact identity for both feeds and in-flight with zero query-struct changes: old tags neither feed nor delay the new round, and a peer can never vote twice in one quorum. The recorded `tag_ids` vector is the coro `shared_ptr<round_state>` closure, reified. Cost: ≤ `consideration_count * 4` hashed lookups per round check.

## Driver loop

`run ()` is unchanged from today (loop `run_one ()` until `ctx.stopped`). `run_one ()`:

```
void frontier_strategy::run_one ():
    channel := null; head_index := 0; position := 0; id := 0

    ctx.wait (predicate):                  # predicate runs under ctx.mutex; capped-backoff poll 5ms -> 100ms
        now := steady_clock::now ()

        # 1. SETTLE — always, never gated. Single settlement point lives inside; runs every poll so
        #    settlement latency is bounded by the poll cap even when launches are blocked.
        ctx.frontiers.settle (now, probes)

        # 2. GLOBAL LAUNCH GATES — pure predicates, consume nothing; re-evaluated every poll, so no
        #    stale-gate window exists (gates and launch share one evaluation)
        if ctx.accounts.priority_half_full ():                            return false
        if workers.queued_tasks () >= ctx.config.frontier_scan.max_pending: return false
        if ctx.tags.size () >= ctx.config.max_requests:                   return false

        # 3. SLOT — capacity-aware probe inside next_launch (constraint 6: probe before tokens).
        #    Busy heads are SKIPPED within this same pass; the walk tries one full rotation.
        slot := ctx.frontiers.next_launch (now, probes)
        if !slot:                                                         return false

        # 4. LIMITER — a token is consumed only with an acquirable slot in hand (15/s budget intact)
        if !ctx.frontier_limiter.should_pass (1):                         return false

        # 5. ACQUIRE + COMMIT — same critical section as the probe: peer_pool mutations require
        #    ctx.mutex, so probe `available` ==> acquire succeeds, except the send-queue residual.
        result := ctx.peers.acquire ({}, slot.exclude)
        if result.status != acquired:
            # Only reachable when every below-capacity candidate's bootstrap send queue is full
            # (probe does not check channel->max). One token burned; bounded by send-queue
            # saturation, never by idle heads. exhausted/no_peers are unreachable by locking.
            debug_assert (result.status == acquire_status::busy)
            stats inc (bootstrap_frontier_scan::busy);                    return false
        id := generate_id ()
        ctx.frontiers.commit (slot.head_index, slot.position, result.node_id, id, now)
        stats inc (bootstrap_next::next_frontier)
        head_index := slot.head_index; position := slot.position; channel := result.channel
        return true

    if !channel: return                    # stopped while waiting; no unlocked ctx.stopped read

    # 6. SEND — off the lock (ctx.send/transmit takes ctx.mutex internally; it inserts the tag
    #    BEFORE channel->send, so a response can never beat the tag, and commit already preceded both)
    query := frontiers_query{ start = position, count = max_frontiers }
    if !ctx.send (channel, query, query_source::frontiers, id):
        # transmit already erased the phantom tag; forget the sample. `used` keeps the peer
        # (implicit penalty until decay) and `launched`/`last_launch` pacing is kept.
        lock ctx.mutex; ctx.frontiers.erase_sample (head_index, id)
```

`engine.settle (now, probes)` — cheap-first sweep; `conclude` is the single settlement function:

```
for each head with a round r:
    ripe := r.votes.settled ()                                       # O(1): quorum or empty-range consensus
    if !ripe:
        pacing_open := r.launched < config.consideration_count
                    or now >= r.last_launch + config.cooldown
        capped := r.launched >= config.consideration_count * max_round_samples_factor
        if pacing_open or capped:
            if probes.count_inflight (r.tag_ids) == 0:               # cheap: <=16 hash lookups
                if capped or probes.peer_status (r.used) == none:    # O(peers) probe LAST, only here
                    ripe := true                                     # exhausted / sample-capped: conclude with what was gathered
    if ripe: conclude (head, now)

conclude (head, now):                                                # the ONLY mutator of cursor/pacing/round lifetime
    r := *head.round
    clean := r.votes.done ()                                         # full quorum with candidates
    if target := r.votes.conclude ():
        stats: done (clean) | done_empty (empty_range) | done_partial (candidates, sub-quorum)
             | done_empty_partial (no candidates, completed > 0: wrap on weak evidence — observable)
        debug_assert (target > head.cursor)                          # heads never regress
        head.processed += r.votes.candidate_count ()
        head.cursor = *target
        if head.cursor >= head.end: stats done_range; head.cursor = head.start   # wrap
    else: stats done_none                                            # nothing learned; cursor stays, position retried
    head.rounds += 1
    head.round.reset (nullptr)                                       # destroy: in-flight ids become stragglers
    if !clean: head.pause_until = now + config.cooldown              # pace partial/empty/none rounds; clean respins immediately
```

`engine.next_launch (now, probes)` — one rotation from `robin`, advanced when a slot is returned:

```
empty_probe := nullopt                                               # memoized peer_status({}) for round-less heads
for one rotation:
    head := heads[(robin + i) % heads.size ()]
    if head.round:
        r := *head.round
        if r.votes.settled (): continue                              # settle owns it; never launch into a settled round
        if r.launched >= consideration_count * max_round_samples_factor: continue   # sample cap
        if r.launched >= consideration_count and now < r.last_launch + cooldown: continue   # paced re-sample
        if probes.peer_status (r.used) == available:
            robin = index + 1; return slot{ index, r.position, r.used }
        # busy: ride out capacity; none: settle's exhausted path owns it — either way, NEXT head
    else:
        if now < head.pause_until: continue
        if !empty_probe: empty_probe = probes.peer_status ({})       # identical for every round-less head
        if *empty_probe == available:
            robin = index + 1; return slot{ index, head.cursor, {} }
return nullopt
```

**Predicate cost, quantified.** Per poll: 128 × O(1) `settled ()`; `count_inflight` only for rounds whose pacing window is open with the eager phase done or in progress; the O(peers × |used|) probe only for (a) rounds with zero in-flight whose pacing window is open (transient: between a response and the next launch/conclusion) and (b) launch-eligible rounds in `next_launch`, plus one shared empty-exclude probe. Worst case under total saturation ≈ 128 × 200 entries ≈ 25 k entry visits per poll; the common case is a handful of probes, because in-flight > 0 covers most of a round's life and the cooldown gates re-checks at 5 s per head past the eager phase. This is an order of magnitude below the critiqued 150 k-comparison shape (which came from running `has_candidate` first); if profiling under mainnet load still shows pressure, the documented contingency is a `peer_pool` epoch counter caching per-round probe results — not needed for correctness.

**Pacing nuances preserved from coro:** a clean quorum respins immediately (`pause_until` untouched); a head with no peers never creates a round and burns nothing (probe-gated, memoized); exhausted conclusion of a paced round is deferred to the cooldown boundary — matching coro, where non-settling wakes did not bypass pacing.

## Response path

Dispatch shape fixed (constraint 3): `bootstrap_context::process ()` finds + erases the tag under ctx.mutex (the derived in-flight count therefore drops at the same instant the sample becomes feedable — atomic, same ordering as coro), verifies the payload type, routes to the strategy **under the lock**, releases the peer slot iff `true`, then `condition.notify_all ()` wakes the driver, whose next `settle ()` concludes any now-ripe round within the poll cap.

```cpp
bool frontier_strategy::process (frontiers_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	auto const & query = std::get<frontiers_query> (tag.query);

	ctx.stats.inc (nano::stat::type::bootstrap_process, response.frontiers.empty () ? nano::stat::detail::frontiers_empty : nano::stat::detail::frontiers);

	switch (verify (response, query))
	{
		case verify_result::invalid:
			ctx.stats.inc (nano::stat::type::bootstrap_verify_frontiers, nano::stat::detail::invalid);
			return false; // Dispatch keeps the peer slot reserved: penalty until pool decay (existing policy)

		case verify_result::ok:
		case verify_result::nothing_new: // CHANGED vs current branch, ported from coro: an empty response
		{ // is a valid "nothing past this position" sample and MUST feed the empty-range consensus
			ctx.stats.inc (nano::stat::type::bootstrap_verify_frontiers, response.frontiers.empty () ? nano::stat::detail::nothing_new : nano::stat::detail::ok);

			bool fed = ctx.frontiers.process (tag.id, query.start, response.frontiers);
			if (!fed)
			{
				ctx.stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::stale); // Straggler: feeds nothing, peer slot still released
			}

			// Ledger reconciliation regardless of round bookkeeping — straggler data is real (coro did the same)
			if (!response.frontiers.empty ())
			{
				ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::frontiers, nano::stat::dir::in, response.frontiers.size ());
				if (workers.queued_tasks () < ctx.config.frontier_scan.max_pending * 4) // Overfill margin, unchanged
					workers.post ([this, f = response.frontiers] { classify (f); });
				else
					ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::frontiers_dropped, response.frontiers.size ());
			}
			return true;
		}
	}
}
```

`engine.process` locates the head by position (floor lookup; ranges are disjoint and fixed), requires `head.round && head.round->owns (tag_id)`; on match it prunes the id from `tag_ids` and calls `votes.feed (frontiers)`. It **never settles** — the response path cannot advance cursors, which is exactly the smeared lifecycle this refactor deletes. A feed landing on an already-`settled ()`-but-not-yet-concluded round (a 5th eager response racing the driver) is deliberately allowed, as in coro: merged candidates can only *lower* the conclude target (the trim keeps the smallest), so it is conservative — pinned by a test.

`classify` (worker thread): ported `classify_frontiers (ledger, tx, frontiers)` off-lock, then `ctx.mutex` + `accounts.priority_set (account, priority_cutoff)` per result, unlock, `notify_all` — today's `process_frontiers` tail with the loop body swapped for the pure function, keeping the `bootstrap_frontiers::{processed, prioritized, outdated, pending}` stats.

**Stats table.** Kept: `bootstrap_frontier_scan::{done, done_empty, done_range}`, `bootstrap_next::next_frontier`, `bootstrap_process::{frontiers, frontiers_empty}`, `bootstrap_verify_frontiers::{ok, nothing_new, invalid}`, `bootstrap::{frontiers (in), frontiers_dropped, processing_frontiers}`, `bootstrap_frontiers::*`. Added to `stats_enums.hpp`: `done_partial`, `done_empty_partial`, `done_none`; reused existing details: `stale` (straggler feeds), `busy` (acquire residual). Retired with the index: nothing externally visible beyond the old `next_*` scheduling internals.

## Edge cases

- **Peer exhaustion / 1-peer dev networks (constraint 5).** Round launches to the only peer (`used = {A}`); probe thereafter returns `none`. While the tag lives, `count_inflight > 0` → settle skips (ride-out; the driver parks on the condvar). Response arrives → tag erased + fed atomically → next settle pass: in-flight 0, probe `none` → exhausted-conclude with one sample → `done_partial`, cursor advances, cooldown paces the next round. System tests advance.
- **Zero peers.** No round is ever created (probe-gated, memoized empty-exclude probe); no limiter tokens consumed; driver parks at the backoff cap. A peer found by maintenance `peers.update ()` is picked up within ≤ poll cap.
- **Busy pool (peers at `channel_limit` / send queues full from other strategies).** Probe returns `busy` → head skipped, rotation continues, no token consumed, no global pause, no per-head deadline park. The only token burn in the whole design is the send-queue residual at step 5 (probe can't see `channel->max`), stat-counted and bounded by send-queue saturation — never by idle heads, and never repeated per notify (the probe gate fails first on subsequent polls).
- **Timeouts.** The (now complete) maintenance sweep erases the tag at its cutoff → derived in-flight drops; the sample never completes (`completed` counts fed samples only). Honest latency bounds: exhausted-conclude after total response loss ≈ `request_timeout` (15 s) + sweep interval (5 s prod / 500 ms dev) + poll cap; up to `request_timeout * 4` (60 s) for a tag whose send-completion callback never fired (its own cutoff was never shortened — inherent per-tag bound, no longer contagious to other tags). Liveness is preserved in all cases; the topo gap-latch inherits these same bounds.
- **Peer churn with universally lost responses.** Without a cap, `used` grows one peer per cooldown forever and the head silently makes zero progress. The sample cap (`consideration_count * 4 = 16`) stops launching and forces a conclude once in-flight drains — `done_partial`/`done_none`, paced, observable.
- **Stale / straggler responses.** Settlement destroys the round; a late response fails `owns (tag_id)` → `stale` stat, no feed, peer slot still released, frontiers still classified. Id-ownership subsumes position-equality (constraint 7) and is strictly stronger: same-position nothing-learned retry rounds reject the predecessor's ids, and the predecessor's still-live tags neither feed nor delay the new round (they are not in its `tag_ids`). Straggler **absorption** (pure-engine's variant) is rejected: it lets one peer hold two of the four quorum votes, and a deliberately slow peer would gain systematic over-representation exactly where the local view is weakest.
- **Sync send failure.** `transmit` erases the tag immediately (callback provably never fires); `erase_sample` prunes the id. `used`/`launched`/`last_launch` kept — intended pacing: a head facing a peer whose sends fail must not machine-gun it, and the eager-budget slot it consumed is the cost of that policy (the round still concludes exhausted/partial and retries paced). Restoring `last_launch` is deliberately rejected.
- **Async send-error callback.** Already erases the tag → automatic decrement; nothing else to do.
- **Hostile payload-type echo.** Dispatch erases the tag and rejects before the strategy runs — derived in-flight stays correct (the scenario that permanently strands a round-internal counter).
- **Race: response vs commit.** Impossible to lose: commit happens under the same lock as probe/limiter/acquire, before `ctx.send`; transmit inserts the tag before `channel->send`; so any response that finds the tag finds a round that owns the id. The committed-id-not-yet-in-tags window is unobservable: only the driver settles, and the driver is busy sending (threading contract in the header; tests mirror the ordering).
- **Exhausted wrap on weak evidence.** `conclude ()` returns `range_end` whenever `completed > 0` and candidates are empty — so on the exhausted path one empty sample (a pruned or lying peer, honest peers timing out) wraps the head's whole remaining band until the next sweep cycle. Inherited from coro and *required* by constraint 5 (1-peer networks must wrap genuinely empty tails). Resolved by observability, not behavior change: `done_empty_partial` distinguishes it from the 2×-quorum `done_empty`; hardening (requiring ≥2 completed samples when the pool has ≥2 peers) is documented as a follow-up gated on re-verifying the 1-peer system tests.
- **`reset ()`.** Under ctx.mutex: rounds destroyed (in-flight requests become stragglers — dropped at dispatch as `missing_tag` or rejected by ownership), cursors → `start`, `pause_until` cleared, counters zeroed. The driver's in-progress turn is safe end-to-end: a reset between commit and a failed send makes `erase_sample` a validated no-op; a reset between commit and a successful send leaves a tag whose response becomes a straggler. No thread restarts.
- **Shutdown.** `stop ()` flips `stopped` under the mutex + `notify_all`; `ctx.wait` unwinds; `run_one` returns via `!channel` (no unlocked `ctx.stopped` read — that was a TSAN-visible race in the draft). A launch that won the predicate just before stop sends once over a possibly-closing channel — bounded, harmless, tag dies with ctx. `stop ()` joins the driver, then `workers.stop ()`.

## Tests

Port the coro `frontier_round` unit tests unchanged (quorum, empty-range at 2×, conclude = largest kept candidate, trim keeps smallest, below-position ignored, nothing-learned → nullopt). Rewrite `nano/core_test/bootstrap_frontier_scan.cpp` around the engine — deterministic, no node, no network, in the existing `test_context` style:

```cpp
struct test_context
{
	nano::stats stats;
	nano::frontier_scan_config config;
	nano::bootstrap::frontier_scan_engine engine;
	std::chrono::steady_clock::time_point now{}; // Virtual clock, advanced explicitly
	std::function<nano::bootstrap::peer_probe_status (std::span<nano::account const>)> peer_status; // Per-test fake
	std::set<nano::bootstrap::id_t> live_tags; // Fake ctx.tags; tests insert ids right after commit (mirrors transmit ordering)
	nano::bootstrap::frontier_scan_engine::probes probes ();
};
```

1. `construction` — heads tile the space; first cursor 1 (burn-account skip); last range ends at max.
2. `eager_fanout` — `consideration_count` slot/commit cycles at one position back-to-back; the next requires `now >= last_launch + cooldown`.
3. `distinct_peers` — after `commit (h, node_a, ...)` the next slot's `exclude` contains `node_a`; the exclude span handed to `peer_status` equals `used`.
4. `quorum_settles_clean` — 4 commits + 4 feeds with candidates → `settle` advances to the largest kept candidate, round destroyed, no pause (clean), `done`.
5. `feed_after_settled_is_conservative` — a 5th feed on a settled-but-unconcluded round only lowers (never raises) the conclude target. *(pins the single-settlement claim's quiet dependency)*
6. `empty_consensus_wraps` — 2× `consideration_count` empty feeds → range end → wrap to start; paced; `done_empty` + `done_range`.
7. `exhausted_partial_conclude` — 1 peer, 1 feed, probe `none`, no live tags → advances with one sample, `done_partial`, cooldown. *(the 1-peer system behavior, isolated)*
8. `exhausted_empty_wraps_with_stat` — 1 empty feed, probe `none` → wrap, `done_empty_partial`.
9. `ride_out_inflight` — probe `none` but one id in `live_tags` → settle no-op, no launch; remove the id → concludes.
10. `busy_skips_head_not_engine` — head 1's exclude probes `busy`, empty exclude probes `available` → the *same* `next_launch` call returns head 2's slot. *(kills the global-pause/rr-starvation class)*
11. `busy_never_concludes` — probe `busy`, in-flight 0 → no settlement, no slot; flip to `available` → launch resumes.
12. `straggler_rejected` — settle a round with a live id; `process (old_id, ...)` returns false; new round unaffected.
13. `retry_round_ignores_predecessor` — nothing-learned conclude with the old tag still live: new round at the same position launches, old id neither feeds (`process` false) nor counts (`count_inflight` sees only new ids). *(decision-B failure mode, both directions)*
14. `erase_sample_basic` — commit + erase: id gone from the span handed to `count_inflight`; peer stays excluded; `last_launch`/`launched` unchanged.
15. `erase_sample_after_reset_noop` — commit → `reset ()` → `erase_sample` is a counted no-op; the head's fresh state is untouched. *(the underflow/UAF class)*
16. `sample_cap_concludes` — always-`available` probe with churning node ids, no feeds, tags expiring each launch: after `consideration_count * 4` launches the round stops launching and concludes `done_none` once in-flight drains.
17. `fairness_and_monotonic_cursors` — round-robin rotation across launchable heads; randomized feed/expiry sequences never regress any cursor except by explicit wrap (property test over the virtual clock).
18. `reset` — open rounds dropped, cursors at `start`, old ids rejected, `container_info` sane.

Probe-before-limiter is **not** an engine test (the limiter lives in the driver; the previous design's test 12 was untestable as specified): it is a structural property of the driver's step order, enforced in code review, observable in systems via `container_info` `limiters/frontier`. System tests `bootstrap.frontier_scan`, `frontier_scan_pending`, `frontier_scan_cannot_prioritize` must pass unchanged with default config (test 7's path guarantees 1-peer advancement; classification of first responses drives prioritization independent of settlement pacing).

## Topo extension

The topo strategy instantiates the same four layers with no redesign. A `topo_scan_engine` in ctx holds **persistent heads** — one spear plus R repair heads, each owning its band cursor (`topo_key`), its page deque, and pacing/gap-latch state (`pause_until` + bounded retry counter) — and a **transient `round_state` per head**: a pure page-quorum core (distinct-peer verdicts on one page at a fixed topo position) plus the identical `used`/`tag_ids`/`launched`/`last_launch`. Everything that made the frontier port safe carries because it never mentions frontiers: id-bound rounds (a page verdict feeds only the round that launched the tag — the tracked-window routing rule from the coro-topo design), tags-derived in-flight (page timeouts auto-decrement, so the gap-pause latch cannot deadlock on a phantom), capacity-aware probe-before-budget (the capability flag is baked into the strategy's probe closure: `ctx.peers.probe (topo_flags, exclude)` — the engine never learns about capabilities), driver-only settlement, exhausted-conclude with cooldown pacing, and the sample cap.

Two committed deltas, decided now so the frontier engine doesn't need an interface break later. First, **settlement outputs**: frontier settlement only moves a cursor, but topo repair-head settlement retires whole pages to the block processor, which must not happen under ctx.mutex inside the wait predicate (constraint 2; cf. the deliberate off-lock `submit_blocks` pattern). Mechanism: `conclude ()` pushes retired pages into an **engine-owned outbox**; the driver's predicate also returns true when the outbox is non-empty, swaps it out under the lock, and submits via `wait_block_processor (strategy::topology, ...)` / the existing `ctx.topology_channel` fair-queue bucket off the lock — the driver remains the only I/O actor. Second, honesty about reuse: the probes/slot **shapes** carry, the **types** are per-engine (`topo_key` positions, a page-verdict core instead of `frontier_round`); this is a template to copy, not a shared interface. The block-processor verdict stream enters as one more pure event method (`engine.block_verdict (...)` under ctx.mutex from the `ledger_notifications` hook, exactly like `process`). The engine test suite is the same eighteen shapes plus outbox-drain and verdict-routing tests.

## Trade-offs of this design vs alternatives considered

- **In-flight from recorded tag ids** vs round-internal counter vs position-keyed `count_tags`: chosen for automatic decrement on every erasure path (including the dispatch payload-type rejection that strands a counter) and exact round identity (position-keying double-counts stragglers and needs a separate feed-correlation field anyway). Cost: the `ctx.send` id-overload and ≤16 hashed lookups per round check, abstracted behind `probes.count_inflight` so tests inject a set.
- **Capacity-aware tri-state probe** vs capacity-blind `has_candidate` + busy-handling in the driver: every busy-handling variant examined (global pause, per-head 250 ms backoff, 1 s fused acquire-park, retry-per-wake) either starved launchable heads or burned the 15/s budget under notify storms. The tri-state probe moves the capacity question to where the launch decision is made, makes acquire-after-probe deterministic within the critical section, and reduces busy handling to "skip this head, try the next, this pass". Cost: one non-reserving O(entries) scan per eligible head, quantified above.
- **Limiter between slot and acquire** vs acquire-first with release-on-dry (zero burn): the only remaining burn is the send-queue residual; acquire-first would eliminate it at the price of acquire/release churn on every dry-limiter poll and inverting the established gate order. (`rate_limiter::size ()` cannot serve as a non-consuming dry-gate: `token_bucket::size ()` does not refill, so it under-reports.) Residual accepted, stat-counted.
- **Settle as an always-run sweep separate from `next_launch`** vs a unified walk (pragmatic): the sweep keeps "settlement latency ≤ poll cap regardless of launch gates" — with a unified walk, settled rounds linger whenever the gates block (a critiqued lingering state). Cost: a second probe call for the rare zero-in-flight exhausted check; bounded by pacing.
- **Stragglers rejected** vs absorbed into same-position retry rounds: absorption softens quorum integrity (one peer, two votes) and rewards adversarially slow peers; rejection matches coro and constraint 7. The only cost is re-asking a peer that already answered a failed round — paced by cooldown.
- **Round created at first commit** vs at round open (coro): identical observable cursor behavior, no object churn for peer-less heads, and "round ⟹ launched ≥ 1" deletes coro's special `launched == 0` pacing branch. The one deliberate, behavior-neutral structural deviation from coro.
- **Engine in ctx** vs strategy-owned: `ctx.reset ()` must work mid-flight without thread restarts; `container_info` wiring exists; sibling symmetry; the coro rationale for strategy ownership (reset = re-enter the root coroutine) has no threaded analog.
- **Backoff polling for cooldowns** vs engine-computed deadlines (`park.wake_at`): the production `ctx.wait` has no deadline overload and the 100 ms cap is blessed by the constraints; a wake-at field the real consumer discards is dishonest API. Deterministic tests advance the virtual clock explicitly instead.
- **Settling in the response path** (rejected outright): saves ≤100 ms of settlement latency by re-introducing dispatch-thread cursor mutation — the exact smeared lifecycle this refactor exists to kill.

## Resolved critiques

Every fatal/major issue from the six critiques, plus the load-bearing minors:

| Critique | Issue | Resolution |
|---|---|---|
| coro-fidelity c0 **major** | Busy-state limiter burn amplified by notify storms (capacity-blind `has_candidate`) | **Adopted the recommended fix**: capacity-aware `peer_pool::probe` for launch eligibility; token consumed only with an acquirable slot in hand; `probe == none` (≡ `!has_candidate`) reserved for the exhausted decision so busy never concludes. Residual burn = send-queue case only, stat-counted, bounded. |
| coro-fidelity c1 **major** | settle/next_launch O(open-rounds × peers) per wake under ctx.mutex | **Adopted**: cheap-first ordering (`settled ()` → `count_inflight` → probe last, only when in-flight 0 and pacing window open), memoized empty-exclude probe per pass, exhausted re-checks paced by cooldown. Bound quantified (~25 k entry visits worst case vs the critiqued 150 k); epoch-cache named as contingency. |
| coro-fidelity c1 **major** | `void settle ()` blocks the topo port (page retirement under the mutex) | **Adopted via outbox**: settlement outputs go to an engine-owned outbox drained by the driver and submitted off-lock; frontier outbox is trivially absent; mechanism committed in the Topo section now. |
| pure-engine c0 **major** | Busy global pause + non-advancing round-robin starves launchable heads | **Structurally absent**: no global pause exists; busy heads are skipped within the same `next_launch` rotation (test 10). `robin` advances on returned slots; every pass visits all heads. |
| pure-engine c0/c1 **major** | `launch_failed (head)` underflows a fresh round after `ctx.reset ()` | **Adopted as validated `erase_sample`**: no-op + stat when the head has no round or the round doesn't own the id (id is globally unique, so no position ambiguity); no counter decrement exists at all (`launched` is deliberately kept). Test 15. |
| pragmatic c0 **major** | `rollback ()` unspecified vs reset (empty-optional deref / underflow) | Same resolution as above — the rollback concept is replaced by validated `erase_sample`. |
| pragmatic c1 **major** | Busy retry loop burns a token per ~1 s cycle; 1 s busy-park head-of-line blocks the driver | **Structurally absent**: no fused acquire-wait with deadline; the tri-state probe means acquire is never attempted against a busy-only pool, and busy heads don't block the rotation. |
| coro-fidelity c0/c1, pragmatic c0/c1 minors | Maintenance sweep head-of-line blocking delays tag erasure (up to ~45–60 s, cross-strategy) | **Adopted the sweep fix**: erase all expired tags per tick (O(≤1000), sanctioned targeted improvement). Residual per-tag 60 s bound for unconfirmed sends documented honestly in Edge cases. |
| coro-fidelity c0 minor | Unlocked `ctx.stopped` read (data race) | **Adopted**: dropped; `!channel` alone covers the stopped path. |
| coro-fidelity c0/c1 minors | `erase_sample`/`commit` robustness; commit takes no position | **Adopted**: `erase_sample` validated no-op; `commit` takes `position` with debug_asserts against cursor/round position. |
| coro-fidelity c0/c1, pure-engine c0/c1 minors | Dead `exhausted`/`no_peers` acquire branches with misleading comments | **Adopted**: collapsed to `debug_assert (busy)` + stat with a comment stating the same-critical-section invariant and the one genuine residual (send queue). |
| coro-fidelity c0 minor | Single-empty-sample wrap on the exhausted path unobservable | **Adopted observability**: `done_empty_partial` stat + explicit Edge-cases documentation; behavioral hardening rebutted for now (constraint 5 requires the 1-peer wrap; coro-accepted delta) and parked as a follow-up. |
| coro-fidelity c1 minor | Commit-to-transmit window invariant undocumented | **Adopted (b)**: explicit threading contract in the engine header (driver-thread-only mutators; commit→transmit is one driver turn) + test fixtures mirror the ordering. The pending-set (a) is rebutted: it adds state to defend against a second settler that the strategy's structure — and the topo template — forbids. |
| coro-fidelity c1 minor | Test 12 (probe-before-limiter) untestable in the engine harness | **Adopted**: removed as an engine test; documented as a driver-structure guarantee, observable via `container_info` limiter gauge. |
| coro-fidelity c1 minor | `launch_slot::exclude` span lifetime misdocumented | **Adopted**: contract corrected to "valid until the next mutating engine call". |
| coro-fidelity c1, pragmatic c1 minors | Missing stat enums; stats story thin | **Adopted**: full kept/added/reused table in Response path; `bootstrap_process`/frontiers-in counters preserved in `strategy::process`. |
| coro-fidelity c1, pure-engine c1 minors | Timeout-conclusion latency understated (prod sweep 5 s, no notify) | **Adopted**: honest bounds in Edge cases (15 s + 5 s + poll cap; 60 s unconfirmed-send case); no sweep notify needed — `ctx.wait` polls at ≤100 ms. |
| pure-engine c0 minor | Straggler absorption double-counts a peer in the quorum | **Decided against absorption** (conflict with pure-engine's design intent): id-ownership rejects stragglers; the adversarial over-representation angle is the deciding argument. |
| pure-engine c1 minor | `park.wake_at` dead in production | **Structurally absent**: no decision variant; polling contract documented in Trade-offs. |
| pure-engine c1 minor | `retry_at` leaks across settlement | **Structurally absent**: no `retry_at` exists; `conclude ()` owns all pacing state it touches (`pause_until`). |
| pure-engine c1, pragmatic c1 minors | Topo "verbatim interface" overstated | **Adopted**: Topo section now states shapes carry, types are per-engine, and commits the outbox + verdict-event mechanisms. |
| pragmatic c0 minor | Unbounded `used` growth under churn with lost responses | **Adopted**: per-round sample cap (`consideration_count * 4`) forcing a paced, observable conclusion. Test 16. |
| pragmatic c0/c1 minors | `last_launch`/`launched` not restored on send failure | **Rebutted with policy**: keeping them is the intended anti-machine-gun pacing (coro behavior); documented in Edge cases and pinned by test 14. |
| pragmatic c0 minor | Test gaps (feed-after-settled, reset-interleave, stale-tag deferral, busy-abandon) | **Adopted**: tests 5, 15, 13, 11 — with 13 noting that tag-id in-flight makes the stale-tag *deferral* itself structurally absent (old tags don't count for the new round). |
| pragmatic c0 minor | Backpressure gates stale across a long park | **Structurally absent**: gates live inside the same predicate evaluation as slot/limiter/acquire/commit — re-checked on every poll. |
| pragmatic c1 minors | C++ nits (const members vs copies, aggregate emplace, vector growth, std::function churn) | **Adopted**: `round_state` explicit ctor; references in walks; `heads.reserve` in the ctor; probes constructed once as a strategy member. |

Key files: `/Users/piotr/WorkNano/nano-node/nano/node/bootstrap/frontier_scan.{hpp,cpp}` (new, ported core), `/Users/piotr/WorkNano/nano-node/nano/node/bootstrap/frontier_scan_engine.{hpp,cpp}` (new, replaces `frontier_scan_index.{hpp,cpp}`), `/Users/piotr/WorkNano/nano-node/nano/node/bootstrap/frontier_strategy.{hpp,cpp}`, `/Users/piotr/WorkNano/nano-node/nano/node/bootstrap/bootstrap_context.{hpp,cpp}`, `/Users/piotr/WorkNano/nano-node/nano/node/bootstrap/peer_pool.{hpp,cpp}`, `/Users/piotr/WorkNano/nano-node/nano/node/bootstrap/queries.cpp`, `/Users/piotr/WorkNano/nano-node/nano/lib/stats_enums.hpp`, `/Users/piotr/WorkNano/nano-node/nano/core_test/bootstrap_frontier_scan.cpp`.