# Bootstrap frontier scan refactor & topo strategy - implementation plan

## Goals & constraints (brief)

1. Refactor the frontier scan in `nano/node/bootstrap` to the **heads + rounds** model from `bootstrap-coro`: the account space is partitioned across heads; each head runs one *round* at a time — the same `frontiers_query` sent to N **distinct** peers, responses merged by a quorum value type, the cursor advanced at a single settlement point. Redundancy is the filter against slow/faulty/malicious peers.
2. **No coroutines.** Plain threads. One driver thread serves all heads round-robin. Keep the classic engine: `bootstrap_context`, the `tags` multi-index as the **single inspectable in-flight registry**, `ctx.mutex` as the one lock, `queries.hpp`/`verify.hpp` policy extraction, `peer_pool` (whose `acquire(capabilities, exclude)` + `has_candidate` + `acquire_status` were added on this branch precisely for fanout rounds).
3. The machinery introduced (passive clock-injected scan container, id-routed sample conclusions, conclusion trichotomy, distinct-peer acquire) must be directly reusable by the phase-2 topo strategy.
4. Priority/dependency/database strategies and the rest of bootstrap are untouched. Changes are additive or local; every commit builds (`.ai/ai_build.sh`) and is test-green (`.ai/ai_test.sh`, both backends).
5. Phase 1 (frontier) is specified at full resolution; phase 2 (topo) at design resolution — it starts only after phase 1 has landed and been evaluated on a live network.

This plan is the synthesis of three independently judged designs plus a three-lens adversarial review. The execution skeleton is the consensus winner ("straight-line": self-settling passive container + one sweeping driver), with grafts from the other two (tag-position keying and commit hygiene from "minimal-delta"; per-sample deadlines, verification-gated straggler accounting, per-head round parametrization for phase 2 from "round-engine"). Every flaw the reviewers identified is resolved in-line below; the two structural fixes are the **erase-on-conclusion sample bookkeeping model** (§ Sample bookkeeping) and the **bounded gate-break wake** (§ Threading).

## Target architecture - phase 1 frontier

```
message_processor thread             driver thread (bootstrap_frontier_scan)        maintenance thread
  ctx.process(ack, channel)            run(): sweep heads round-robin                 sweep expired ctx.tags
    find tag by id ─ erase               drain frontiers.expired → conclude_tag         conclude(tag, timeout)
    payload type check                   gates → throttle_wait poll                       └→ frontiers.timeout(id)
    frontier_strat.process               frontiers.next(now) → intent                   peers.update; notify_all
      verify(payload, query)             probe peers.has_candidate({}, used)
      frontiers.process(id, ...)         limiter → peers.acquire({}, used)            ASIO send callback
        erase sample → feed round        frontiers.launched(intent, id, node_id, now)   ok  → confirm deadline
        → settle → advance               [unlock] ctx.send(channel, query, src, id)     err → conclude(tag, failure)
      post classify → workers(1)         [relock]                                           └→ frontiers.failure(id)
        └→ accounts.priority_set       wait_for(clamp(wake - now, 0ms, 1s))
```

How it differs from today:

| Today (`frontier_scan_index`) | After |
|---|---|
| Implicit redundancy: up to 4 *overlapping* requests per head, all to whatever peer `peers.acquire()` returns (possibly the same one), merged statistically on the head | Explicit round: N samples at one fixed cursor to N **distinct** peers (exclusion-list acquire), merged by a pure `frontier_round`, settled exactly once |
| Head lookup by `upper_bound(start)` — ambiguous across round boundaries | Samples routed by wire id → `(head, round_seq)`; stragglers from settled rounds detected exactly |
| Timed-out tags silently vanish; the head heals only via cooldown re-eligibility | Every tag concludes through exactly one of response / timeout / failure; rounds always drain |
| Empty responses early-return, contributing nothing | Empty verified responses are *votes* that nothing exists past the cursor (enables unanimous-empty wrap) |
| Index reads `steady_clock` internally (forces `sleep_for(500ms)` in tests) | All time injected as `now` parameters; the scan container is fully deterministic and synchronous-testable |
| `ctx.wait` 5–100 ms backoff polling, always | Deadline-accurate `wait_for` with a 1 s safety cap; only gate-closed periods poll, at the existing `throttle_wait` (100 ms) cadence — same cost as today's cap |

What deliberately does **not** change: `ctx.mutex` as the only lock; `ctx.tags` as the only in-flight registry; the response spine (`message_processor → ctx.process → tag lookup → payload verifier → strategy`); peer accounting (release only on verified-ok/nothing-new response; invalid/timeout/send-failure keep the slot reserved as an implicit penalty until `peer_pool::decay()` — comment at `bootstrap_context.cpp:181`); classification on the strategy-private 1-thread pool; shutdown ordering; request economics (≈4 requests per head advance, `frontier_rate_limit` 15/s — same cost as today, just explicit and observable).

## Detailed design

### Components

**NEW `nano/node/bootstrap/frontier_scan.{hpp,cpp}`** (replaces `frontier_scan_index.{hpp,cpp}`, deleted at cutover; add to `nano/node/CMakeLists.txt`; new types registered in the bootstrap forward-declaration header per CLAUDE.md). Three things, all caller-synchronized by `ctx.mutex` (documented like `peer_pool`), all clock-injected — no internal `steady_clock::now()`, no network, no threads:

1. **`frontier_round`** — ported verbatim from `bootstrap-coro:nano/node/bootstrap/frontier_scan.{hpp,cpp}` (verified pure):

```cpp
// Candidate voting for a single frontier scan round at a fixed position. Pure value type; lifetime = one round.
class frontier_round
{
public:
	frontier_round (nano::frontier_scan_config const &, nano::account position, nano::account range_end);
	// Merges one completed sample; only accounts > position become candidates; an empty sample still counts
	// towards the consensus that nothing exists past the position
	void feed (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);
	bool done () const;        // completed >= consideration_count && !candidates.empty ()
	bool empty_range () const; // completed >= 2 * consideration_count && candidates.empty ()
	bool settled () const;
	// largest kept candidate | range_end (unanimous empty -> wrap) | nullopt (nothing learned -> retry position)
	std::optional<nano::account> conclude () const;
	size_t completed () const;
	size_t candidate_count () const;
private:
	std::set<nano::account> candidates; // truncated to the smallest config.candidates entries, erasing largest
	size_t completed_m{ 0 };
};
```

The smallest-N truncation + advance-to-largest-kept invariant is the anti-skip/anti-poison core: a far-ahead fabricated page cannot push the cursor past frontiers the slowest honest peer attested. Implementation note: `frontier_round` holds a config reference and const members, so it is not assignable — round storage in the scan manages it via `std::optional<round_state>::emplace()/reset()`, never assignment.

2. **`classify_frontiers`** — the ledger reconciliation currently inlined in `frontier_strategy::process_frontiers` (`frontier_strategy.cpp:159-247`), extracted as the coro branch already did:

```cpp
struct frontier_classification { std::deque<nano::account> prioritize; size_t outdated{ 0 }; size_t pending{ 0 }; };
// Deterministic over the ledger snapshot; frontiers must be ascending by account
frontier_classification classify_frontiers (nano::ledger &, nano::secure::transaction const &, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);
```

3. **`frontier_scan`** — heads + open rounds + id→round routing. Passive and **self-settling**: rounds are fed and settled by whichever caller delivers the concluding event (under `ctx.mutex`); the driver only launches. Production call sequences equal test call sequences.

```cpp
class frontier_scan
{
public:
	frontier_scan (frontier_scan_config const &, nano::stats &);

	struct sample_intent
	{
		size_t head;
		uint64_t round_seq;                // generation of the round the intent belongs to
		nano::account start;               // == head cursor == frontiers_query.start
		std::vector<nano::account> used;   // VALUE SNAPSHOT of node ids already sampled this round (acquire exclusion list); <= ~consideration_count entries
		bool inflight;                     // any samples of this round still pending
	};
	// Next launchable sample, rotating round-robin across heads from an internal rotor; opens a round at the
	// head cursor when none is open (round_seq bumps at open, NOT per retry). nullopt when no head is eligible.
	std::optional<sample_intent> next (std::chrono::steady_clock::time_point now);

	// Driver feedback on an intent — called BEFORE the wire send, same critical section as next()
	void launched (sample_intent const &, id_t id, nano::account const & node_id, std::chrono::steady_clock::time_point now);
	void exhaust (size_t head, std::chrono::steady_clock::time_point now); // no distinct peer left, nothing inflight: settle with what was gathered
	void stall (size_t head);                                              // no distinct peer left, samples inflight: park until a conclusion routes to this head

	// Sample conclusions — the outcome trichotomy. Every conclusion FIRST erases the samples entry for the id
	// (unknown id => strict no-op + stat), THEN routes by the erased snapshot's (head, round_seq):
	// fresh => feed / decrement inflight, may settle inline; stale => straggler stat only.
	// Any conclusion routed to a known sample clears its head's stalled flag.
	bool process (id_t id, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers, std::chrono::steady_clock::time_point now); // false when not fed to an open round
	void timeout (id_t id, std::chrono::steady_clock::time_point now);
	void failure (id_t id, std::chrono::steady_clock::time_point now);
	// Send confirmed by the channel: response deadline shrinks from launch + 4x to confirm + 1x request_timeout.
	// Applies to any live samples entry regardless of round_seq (stragglers drain on schedule too);
	// unknown id => strict no-op + stat (reachable when reset() races the send-success callback)
	void confirm (id_t id, std::chrono::steady_clock::time_point deadline);

	// Samples past their deadline — open-round and straggler entries alike. Each id is returned at most once
	// overall, because the conclusion the caller dispatches for it erases the entry.
	std::deque<id_t> expired (std::chrono::steady_clock::time_point now) const;

	std::chrono::steady_clock::time_point deadline () const; // earliest wake: head cooldowns, pacing, sample deadlines
	size_t total_inflight () const;
	void reset ();
	nano::container_info container_info () const; // per-head progress in [0, 1e6] (port cpp_dec_float_50 math), open_rounds, samples, stalled

private:
	struct round_state
	{
		frontier_round round;
		std::vector<nano::account> used;
		size_t inflight{ 0 };
		size_t launched{ 0 };
		std::chrono::steady_clock::time_point last_launch{};
	};
	struct frontier_head
	{
		nano::account const start, end; // [start, end), 1/head_parallelism of the account space (layout unchanged)
		nano::account cursor;
		std::optional<round_state> round; // emplace()/reset() only — round_state is not assignable
		uint64_t round_seq{ 0 };
		bool stalled{ false };
		std::chrono::steady_clock::time_point resume_at{};
		size_t processed{ 0 };
	};
	struct sample_info { size_t head; uint64_t round_seq; std::chrono::steady_clock::time_point deadline; };

	void settle (frontier_head &, std::chrono::steady_clock::time_point now); // the single settlement point; does NOT touch `samples`

	frontier_scan_config const & config;
	nano::stats & stats;
	std::vector<frontier_head> heads; // const start/end => constructed once, never assigned or resized; the boost multi_index is dropped
	std::unordered_map<id_t, sample_info> samples;
	size_t rotor{ 0 };
};
```

`sample_intent` is a **pure value snapshot** (the `used` list is copied, ≤ ~4 accounts of 32 bytes at ≤15 launches/s — trivially cheap): nothing in it can dangle across `launched()`, `exhaust()`, or the driver's unlock seam. It may become *stale* (the world changed), which is why the driver re-fetches via `next()` after any unlock, but it can never be a use-after-free. This snapshot contract carries unchanged into the phase-2 topo driver.

### Sample bookkeeping (the straggler model — normative)

`samples` is the id → `(head, round_seq, deadline)` map. Settle-with-inflight is the **normal** path (3 of 4 eager samples respond and reach quorum while the 4th is pending — exactly the model in the coro source being ported), so the lifecycle rules are stated once, normatively:

1. `launched()` inserts the entry (deadline = launch + 4×`request_timeout`; `confirm()` shrinks it to confirm + 1×).
2. **Every conclusion (`process`/`timeout`/`failure`) erases the entry unconditionally as its first step**, then routes by the erased snapshot: `round_seq` matches the head's open round ⇒ *fresh* (feed / decrement `inflight`, may settle inline); mismatch or no open round ⇒ *straggler* (stat only, nothing else). Unknown id ⇒ strict no-op + stat (`unknown_id`).
3. `settle()` does **not** touch `samples`. A round settling with samples still in flight leaves those entries behind as stragglers; each is removed by exactly one later conclusion — its response, its send-failure, the scan's `expired()` sweep, or the maintenance cutoff sweep, whichever fires first.
4. Consequently `expired()` can never re-return an id: the timeout conclusion it triggers erases the entry. No driver spin is possible from stale entries.
5. The internal-consistency invariant is `Σ open-round inflight == #{samples entries whose round_seq matches their head's open round}` (debug_assert). `samples.size()` exceeds that sum by exactly the straggler count — this is expected, not an error.
6. `reset()` clears `samples` (and re-creates heads); surviving in-flight tags conclude later as unknown ids per rule 2.
7. **Stalled clearing, stated once**: any conclusion that routes to a known sample of head H — fresh *or* stale, success *or* failure — clears H's `stalled` flag. (A conclusion means a peer slot may have freed; the next sweep re-probes. The "timeout/failure only decrement inflight and clear stalled" phrasing elsewhere contrasts them with `process`, which *additionally* feeds — all three clear `stalled`.)

**REWRITTEN `nano/node/bootstrap/frontier_strategy.{hpp,cpp}`** — same class shape (one `std::thread`, private `nano::thread_pool workers{1}` for classification, `start/stop/process`). `run_one` becomes the driver sweep (below); `process` is rewritten (empty responses feed; id-routed; stragglers still classified); gains `timeout (async_tag const &)`, `failure (async_tag const &)`, `confirm (async_tag const &, time_point)` forwarders into the scan. The classification completion path (`process_frontiers`) notifies `ctx.condition` **unconditionally** (today it notifies only `if (!result.empty ())`, frontier_strategy.cpp:243-246 — an all-empty batch would leave the queued_tasks-gated driver to the 1 s cap).

**MODIFIED `nano/node/bootstrap/bootstrap_context.{hpp,cpp}`** (local, additive):

- Member swap `frontier_scan_index frontiers` → `frontier_scan frontiers` (include swap in the header).
- **Conclusion trichotomy**: a `conclude` helper becomes the choke point — the rule is *every tag-erase path other than response consumption dispatches through it*:

```cpp
enum class conclusion { timeout, failure };
void conclude (async_tag const & tag, conclusion);  // routes by tag.source; frontiers -> frontier_strat.timeout/failure; no-op for other sources; emits the engine-wide timeout/failure stats
bool conclude_tag (id_t, conclusion);               // lookup tag by id + conclude + erase; false if the tag is already gone (caller-locked; used by the driver sweep)
```

Dispatch sites — five land in commit 1, the sixth at cutover (commit 6); this enumeration is the review checklist:
1. **Maintenance sweep** (`maintenance()`, cpp:431-439): for each expired tag, `conclude(tag, timeout)` before `pop_front`; `condition.notify_all ()` at the end of every maintenance pass (also propagates `peers.update` results to a driver parked on an empty pool — `peers.update` runs inside the same pass, cpp:419).
2. **ASIO send-callback error branch** (cpp:213-217): `conclude(tag, failure)` before erasing.
3. **`transmit` synchronous send failure**: when `channel->send` returns `false`, erase the just-inserted tag and `conclude(tag, failure)`. Verified real bug: `tcp_channel::send_impl` (tcp_channel.cpp:91) returns `false` on a full queue **without ever invoking the callback**, so today such tags linger for `4 × request_timeout` (60 s) — this is an engine-wide fix benefiting all strategies.
4. **`process()` `invalid_response_type` early-return** (cpp:498-502): the tag is already erased; add `conclude(tag, failure)` before returning. Without this, a peer answering with a mismatched payload type would leak the scan's sample entry forever (modulo the deadline backstop).
5. **Strategy-level `verify_result::invalid`**: `frontier_strategy::process` calls `ctx.frontiers.failure (tag.id, now)` itself (the tag was consumed by `process()`; only the scan-side conclusion is needed).
6. **Driver expired-sweep** (commit 6): the driver drains `frontiers.expired()` through `conclude_tag (id, timeout)` — same helper, same engine-wide `bootstrap_timeout` stats as the maintenance path (cpp:436-437), so scan-side timeouts do not vanish from the shared counters the soak comparison depends on. If the tag is already gone, the driver concludes scan-side only (`frontiers.timeout (id, now)`).
- `send()` gains a pre-generated id parameter: `bool send (channel, query_descriptor, query_source, id_t id = generate_id ())`; `async_tag.id` is assigned from it instead of self-initializing. This lets the driver **record the sample before the send** (closes the response-before-recorded race in the record-after alternative).
- Send-success callback (cpp:206-211), in addition to shrinking the tag cutoff, dispatches `confirm` for frontier-source tags → `frontiers.confirm (id, now + request_timeout)`. The callback body only runs while the tag exists (verified: cpp:202-218), but `reset()` can clear `samples` while in-flight tags survive — hence `confirm`'s unknown-id strict-no-op contract above.
- `payload_verifier`, peer release-on-ok in `process()`, `wait`/`wait_channel` for the other strategies, `inspect`, `submit_blocks`, `reset()` shape: untouched. `reset()` now resets the new container.

**MODIFIED `nano/node/bootstrap/queries.cpp`** (one line): `index_keys (frontiers_query)` returns `{ query.start, 0 }` instead of `{0, 0}` (queries.cpp:71-74). Verified inert — `count_tags` filters by source and nothing keys frontier tags by account today. Value: frontier tags in `ctx.tags` become inspectable by scan position while debugging (`count_tags(position, query_source::frontiers)` from inside the engine / a debugger). Note `count_tags` has no external test seam today (it debug_asserts the held mutex and `ctx` is private to `bootstrap_service`), so the commit-2 test asserts `index_keys` directly as a pure function; an external accessor is added only if and when something consumes it.

**MODIFIED `nano/lib/stats_enums.hpp` + `nano/node/bootstrap/common.{hpp,cpp}`**: new `stat::detail` entries under `stat::type::bootstrap_frontier_scan` (below).

**UNCHANGED**: `peer_pool` (its `acquire (caps, exclude)` exclusion list, `has_candidate` probe and `busy/exhausted/no_peers` statuses get their first real user), `verify.{hpp,cpp}` (frontiers verify already returns `nothing_new` for empty, `invalid` for unsorted/below-start — exactly the contract the round feed needs), `account_sets_index`, priority/database/dependency strategies, `bootstrap_config` (**zero new keys**: `head_parallelism=128`, `consideration_count=4` (quorum + eager fanout; ×2 = empty-range threshold), `candidates=1000` (trim bound), `cooldown=5s` (re-sample pacing + non-clean respin), `max_pending=16` (classification gate), `frontier_rate_limit=15`, `request_timeout=15s`, `throttle_wait=100ms` keep their meanings).

### Threading & synchronization

No new threads, no new mutexes. `ctx.mutex` remains THE lock and now protects `frontier_scan` exactly as it protected `frontier_scan_index`. Thread census for frontier work:

- **Driver** (`thread_role::bootstrap_frontier_scan`, 1 thread, all 128 heads):

```cpp
void frontier_strategy::run ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_frontiers);
		auto wake = run_some (lock); // sweep; drops the lock only around sends
		auto now = std::chrono::steady_clock::now ();
		// 1s cap = safety net against missed notifies; common-type clamp (duration literals mix otherwise)
		ctx.condition.wait_for (lock, std::clamp<std::chrono::steady_clock::duration> (wake - now, 0ms, 1s));
	}
}
```

- **The sweep**, with the explicit unlock seam (`ctx.send → transmit` takes `lock_guard{ctx.mutex}` at bootstrap_context.cpp:192, so the driver must not hold the lock across it) and **bounded wakes on every exit path** — no exit may return a perpetually-past deadline:

```cpp
std::chrono::steady_clock::time_point frontier_strategy::run_some (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());
	auto now = std::chrono::steady_clock::now ();
	while (!ctx.stopped)
	{
		// Conclude expired samples through the engine choke point (shared bootstrap_timeout stats + scan
		// conclusion). Each id is drained at most once: the conclusion erases its samples entry.
		for (auto id : ctx.frontiers.expired (now))
		{
			if (!ctx.conclude_tag (id, bootstrap_context::conclusion::timeout))
			{
				ctx.frontiers.timeout (id, now); // tag already gone; conclude scan-side only
			}
		}
		// Backpressure gates: return a BOUNDED wake (throttle_wait poll, matching today's ctx.wait cap).
		// frontiers.deadline() must NOT be returned here — launch-eligible heads make it <= now and the
		// driver would spin at 100% CPU on a sustained gate (priority_half_full holds for hours on a real
		// bootstrap). Event notifies (classification drain, tag erase) wake us sooner where they exist;
		// the priority gate has no drain notify and is governed by this poll, exactly like today.
		if (ctx.accounts.priority_half_full ()) return now + ctx.config.throttle_wait;
		if (workers.queued_tasks () >= ctx.config.frontier_scan.max_pending) return now + ctx.config.throttle_wait;
		if (ctx.tags.size () >= ctx.config.max_requests) return now + ctx.config.throttle_wait;

		auto intent = ctx.frontiers.next (now);
		if (!intent) break; // all heads parked; frontiers.deadline () says when to recheck

		// Probe before spending a limiter token other heads need
		if (!ctx.peers.has_candidate ({}, intent->used))
		{
			if (intent->used.empty ())
			{
				// Pool-wide: NO capable peer exists (used empty => nothing head-specific excluded).
				// Leave the head's round untouched (no exhaust, no round_seq churn, no 5s cooldown park)
				// and re-probe on the maintenance notify (peers.update runs there) or this bounded poll.
				return now + ctx.config.throttle_wait;
			}
			// Head-specific: every remaining capable peer was already sampled this round
			intent->inflight ? ctx.frontiers.stall (intent->head) : ctx.frontiers.exhaust (intent->head, now);
			continue; // stall/exhaust mutate the head, so the sweep provably terminates
		}
		if (!ctx.frontier_limiter.should_pass (1))
		{
			return now + limiter_retry; // 25ms constant
		}
		auto result = ctx.peers.acquire ({}, intent->used);
		if (result.status != peer_pool::acquire_status::acquired)
		{
			// After a successful has_candidate probe in the same critical section, only `busy` is reachable
			// (verified peer_pool.cpp:15-67: candidate_present is necessarily true) — exhausted/no_peers
			// are structurally dead here.
			debug_assert (result.status == peer_pool::acquire_status::busy);
			// Deliberately coarse: busy is exclusion-list-relative, so another head might still acquire,
			// but a <=100ms recheck beats per-head busy bookkeeping and a continue here would not mutate
			// the head, risking a non-terminating sweep. The limiter token spent just above is accepted
			// as lost (rare; ~67ms of budget). Revisit on soak if `busy` stats say otherwise.
			return now + ctx.config.throttle_wait;
		}
		auto id = generate_id ();
		frontiers_query query{ intent->start, nano::messages::asc_pull_ack::frontiers_payload::max_frontiers };
		ctx.frontiers.launched (*intent, id, result.node_id, now); // record BEFORE send; same critical section as next()
		lock.unlock ();
		ctx.send (result.channel, query, query_source::frontiers, id);
		// Synchronous send failure: transmit has already erased the tag and dispatched
		// conclude(failure) -> frontiers.failure(id), rolling the registration back. No special path needed.
		lock.lock ();
		now = std::chrono::steady_clock::now ();
		// The world may have changed across the unlock; the loop re-fetches via next(). (intent itself is a
		// value snapshot and cannot dangle — staleness, not lifetime, is the reason to re-fetch.)
	}
	// All heads parked: every per-head wake (resume_at, pacing, stall->sample deadline) is in the future by
	// construction. A sample expiring DURING the sweep can make this <= now once; that costs exactly one
	// immediate re-loop which drains it at the top — terminating, not a spin.
	return ctx.frontiers.deadline ();
}
```

Key discipline points: `next() → gates → probe → acquire → launched()` happen in **one** critical section (no settle can interleave; the `round_seq` in the intent is asserted by `launched()` as cheap insurance); the lock is dropped **only** around `ctx.send`; after relock, the intent is discarded and the sweep re-fetches. Record-before-send + tag-insert-before-wire-send (existing `transmit` order) means a response can never arrive before both registries know the sample.

**Empty-pool behavior** (startup, peer churn): the pool-wide probe-failure path above never settles rounds, so there is no round_seq churn and no 5 s cooldown park — heads keep their open zero-launch rounds and the driver re-probes within `throttle_wait`, or immediately on the maintenance `notify_all` that follows `peers.update` (the only place pool membership changes — 500 ms dev / 5 s prod cadence, same visibility as today's `wait_channel` polling, so the `bootstrap.frontier_scan*` 10 s `ASSERT_TIMELY` margins are unchanged). If a head's round dies entirely while the pool drains (all samples timeout, no distinct peer), the zero-completion exhaust closes it with a fresh `used` (§ Settlement), and the *next* round's empty `used` routes it onto this pool-wide park — heads converge here after at most one retry cycle.

- **Wake sources** (all already exist or are added in commits 1/6): `ctx.process` `notify_all` (sample concluded / round settled / tag budget freed); maintenance `notify_all` (timeout conclusions dispatched, peers updated); send-failure conclusion (callback already locks; add notify); unconditional classification-completion notify (queued_tasks gate reopens); deadline expiry; the bounded polls (`throttle_wait` on gates/busy/no-pool, 25 ms on limiter); the 1 s cap. A missed edge costs ≤ 1 s, never a hang; a closed gate costs a 100 ms poll, never a spin. The `loop_frontiers` stat rate is the spin canary — watched explicitly in commits 7–8.
- **Message-processor / maintenance / ASIO callback threads** enter under `ctx.mutex` exactly as today; they additionally feed/settle rounds (one extra frame: `ctx.process → frontier_strat.process → frontiers.process → settle`). Settlement inline on the delivery thread is deliberate: the mutex already serializes everything, no new thread orderings are introduced, and advance latency is decoupled from driver wake timing. Round merge cost is O(≤1000·log) per response at 15 resp/s — negligible; the pre-existing hot spots (inspect-under-tx) remain the ceiling and are out of scope.
- **Classification worker** (strategy-private pool, 1 thread): `classify_frontiers` inside one read tx, no `ctx.mutex`; re-locks only to `priority_set` — unchanged shape, plus the unconditional completion notify.
- **Shutdown**: unchanged ordering (`ctx.stop()`: `stopped` + notify → strategy `stop()` joins driver then `workers.stop()` → maintenance join → ctx workers). Round state is plain data; nothing to cancel. Post-stop acks are passive mutex-guarded feeds, safe under the existing node teardown order (bootstrap stops at node.cpp before network/block_processor destruction).

Throughput: up to 128 × 4 = 512 concurrent samples (within `max_requests` 1000), paced by the 15/s limiter — identical request economics to today; the limiter, not the single thread, is the binding constraint.

### Round lifecycle

**Head selection** — `next(now)` scans heads from `rotor`, wrapping once, advancing the rotor past a returned head (strict round-robin fairness; deterministic for tests). Per head:

- *Skip*: `stalled`; no round and `now < resume_at`; open round with `launched >= consideration_count && now < last_launch + cooldown` (pacing: first 4 samples eager, re-samples of a stuck round cooled 5 s).
- *Eligible*: no round and `now >= resume_at` → open `round_state` at `cursor` (bump `round_seq` — once per round, not per sweep); or open unpaced round. Return `sample_intent{head, round_seq, cursor, used-copy, inflight > 0}`.

**Fanout**: consecutive intents for one head carry a growing `used` snapshot; `peers.acquire({}, used)` guarantees each sample of a round lands on a distinct node id. Same `frontiers_query{cursor, 1000}` to each — redundancy at one position is the filter.

**Gathering**: every conclusion routes by id per the Sample bookkeeping rules (erase first; fresh feeds, stale is a straggler stat, unknown is a no-op stat). A verified response (`ok` or empty `nothing_new`) feeds the round; `timeout`/`failure` decrement `inflight` only. All three clear the head's `stalled` flag when routed.

**Settlement** — `settle()` is the only cursor writer; invoked from `process/timeout/failure/exhaust` when `round.settled()` holds **or** the round is being exhausted with `inflight == 0`. It closes the round (`optional::reset()`) and never touches `samples`:
- `conclude()` → target: `cursor = target` (debug_assert strictly increasing), wrap to `start` when `cursor >= end`; stat `done` (clean quorum) or `done_partial` (exhaust).
- `conclude()` → `range_end` (completed > 0, zero candidates): wrap; stat `done_empty`.
- `conclude()` → `nullopt` (zero completions — e.g. every sample timed out): cursor stays, round **closes anyway**, `used` is destroyed with the round, `resume_at = now + cooldown`; stat `retry`. **The head-wedge fix**: an exhausted zero-completion round must not keep its stale exclusion list alive — the retry round starts with a fresh `used`, so a small or degraded peer set can always be re-sampled.
- Non-clean settles (`!done()`) set `resume_at = now + cooldown` so partial/wrapping heads don't monopolize the 15/s budget; a clean quorum respins immediately.

**Adversarial cases**:
- *Duplicate response* (same peer answers twice): tag erased on first consumption → `missing_tag`, never reaches the scan — one peer cannot double-vote.
- *Straggler* (round settled, `round_seq` stale): full verification still runs in the strategy **before** any peer accounting (`ctx.process` releases only on the strategy returning ok), so a malicious straggler cannot launder its slot penalty with a late garbage reply — verification-gated release for free, by layering. A verified non-empty straggler is still classified (the data is paid for); it never feeds a round; its `samples` entry is erased on arrival (or by `expired()`/maintenance if it never arrives).
- *Partial* (fewer than 4 reachable distinct peers): `exhaust` settles with whatever completed — quorum adapts down; single-peer networks advance on the first reply (behavioral improvement over today's 4-overlapping-requests-to-one-peer).
- *Malicious page* (unsorted / below start): `verify → invalid` → no feed, `failure(id)` concludes the sample, strategy returns false → peer slot stays reserved (implicit penalty until `decay()`).
- *Slow/dead peer*: sample concludes at its deadline (scan-side `expired()` sweep through `conclude_tag`) or at the maintenance cutoff sweep, whichever fires first — both idempotent by id-lookup under the one mutex; slot stays reserved; the round re-samples a distinct peer after pacing.

### Response routing

Unchanged spine: `message_processor` → `bootstrap_context::process (ack, channel)` → lock → find tag by random 64-bit wire id in `ctx.tags` → erase (duplicate ⇒ `missing_tag`) → `payload_verifier` type check (mismatch ⇒ `conclude(failure)` + drop) → `std::visit` → `frontier_strategy::process (frontiers_payload, tag)` under the mutex. The strategy: extract `frontiers_query` from `tag.query`, `verify` →
- `ok`: `frontiers.process (tag.id, response.frontiers, now)` (feed or straggler) + post classification (existing 4×-overfill-then-drop rule at frontier_strategy.cpp:131-141); return true → peer released.
- `nothing_new` (empty): `frontiers.process (tag.id, {}, now)` — **an empty vote**, the behavior change vs today's early-return at frontier_strategy.cpp:113-117; return true.
- `invalid`: `frontiers.failure (tag.id, now)`; return false → no release.

`queries.hpp`/`verify.hpp` are reused untouched except the one-line `index_keys` change.

### Failure handling

- **Timeouts, two layers**: (a) the scan's own per-sample deadline (`launch + 4×request_timeout`, shrunk to `confirm + request_timeout` by the send-success `confirm` dispatch — mirroring the existing two-phase tag cutoff), folded into `deadline()` and swept by the driver **through `conclude_tag`** (engine stats included); (b) the maintenance cutoff sweep dispatching `conclude(tag, timeout)` at its 500 ms/5 s cadence. Both are idempotent by id-lookup, in either order. Layer (a) is also the **structural leak backstop**, and it covers *all* recorded samples — open-round and straggler alike: even if some future tag-erase path forgets its conclusion dispatch, a sample cannot outlive its deadline — the scan self-heals. Plus the internal-consistency debug_assert from § Sample bookkeeping rule 5.
- **Send failures**: synchronous drop and async callback error both erase the tag and dispatch `conclude(failure)`; the peer slot stays reserved (existing penalty rule).
- **`reset()`**: re-creates heads at range starts, clears `samples`; surviving tags conclude later as unknown ids (strict no-op + stat). Node restart: heads restart at range starts — same as today, harmless because the scan is cyclic.

### Config & stats

No new or changed config keys in phase 1 (`throttle_wait=100ms` already exists and is reused for the bounded gate/busy/no-pool wakes). `limiter_retry` = 25 ms compile-time constant; the `2×consideration_count` empty-range threshold stays a derived constant.

New `stat::detail` under `stat::type::bootstrap_frontier_scan`: `sample` (launched), `done`, `done_partial`, `done_empty`, `retry`, `straggler`, `exhaust`, `stall`, `timeout`, `unknown_id`; existing `bootstrap_verify_frontiers::{ok,nothing_new,invalid}`, `bootstrap_frontiers::{processed,prioritized,outdated,pending}`, `frontiers_dropped`, `bootstrap_timeout`, `bootstrap_tag_duration` unchanged — and still fed by *both* timeout layers, since the driver sweep routes through `conclude_tag`. A debug log line at each settlement (head, cursor → target, completed, candidates, clean/partial). `container_info`: per-head progress scaled to [0, 1e6] (port the cpp_dec_float_50 math from `frontier_scan_index.cpp`), plus `open_rounds`, `samples` (includes pending stragglers), `stalled`.

## Commit sequence - phase 1

Every commit: build `.ai/ai_build.sh 2>&1 | tail -10`, format `.ai/ai_format.sh --filter=...`, tests as listed; full suite both backends at commits 1, 6, 8.

1. **"Conclude bootstrap requests through per-source outcome handlers"** — the conclusion trichotomy plumbing: private `conclude(tag, timeout|failure)` helper; dispatch from the maintenance sweep, the ASIO callback error branch, the new `transmit` synchronous-drop erase (real bug fix: dropped sends currently pin a tag for 60 s), and the `invalid_response_type` early-return (sites 1–4; site 5 is strategy-internal, site 6 — the driver sweep via `conclude_tag` — lands at cutover, completing the six-site enumeration); `notify_all` at end of every maintenance pass; no-op frontier handlers for now. Behavior-neutral apart from the fixes. Files: `bootstrap_context.{hpp,cpp}`, `frontier_strategy.{hpp,cpp}` (stub forwarders). Verify: full suite.
2. **"Key frontier query tags by scan position"** — `index_keys(frontiers_query) → {query.start, 0}` + a pure `index_keys (frontiers_query{start}) == {start, 0}` assertion in `nano/core_test/bootstrap_policy.cpp` (which already tests the extracted policy functions; `count_tags` itself has no external test seam and is not claimed as one). Inert. Files: `queries.cpp`, `bootstrap_policy.cpp`.
3. **"Extract frontier classification into a pure function"** — new `frontier_scan.{hpp,cpp}` containing `classify_frontiers` + `frontier_classification` (port from `bootstrap-coro`); `frontier_strategy::process_frontiers` delegates; ledger-fixture tests. Files: new pair, `frontier_strategy.cpp`, `nano/node/CMakeLists.txt`, tests.
4. **"Add frontier_round candidate voting"** — port `frontier_round` into `frontier_scan.{hpp,cpp}` + pure unit tests (quorum done, 2× empty-range, smallest-N truncation, empty-vote counting, conclude trichotomy). Unused in production.
5. **"Add round-based frontier scan container"** — the passive `frontier_scan` class (heads, samples, self-settling, `expired`/`confirm`/`deadline`, the § Sample bookkeeping model) + new stat enums + a deterministic synchronous test suite in a new `core_test/bootstrap_frontier_rounds.cpp` (modeled on the 27-test `topo-bootstrap-clean-slate:bootstrap_topo_scan.cpp` precedent; full case list in the Test plan, including the settle-with-inflight straggler and expired-straggler cases). Coexists with the old index, unused in production. Files: `frontier_scan.{hpp,cpp}`, `stats_enums.hpp`, `common.{hpp,cpp}`, test file, `core_test/CMakeLists.txt`.
6. **"Drive frontier scan with fanout rounds"** — the cutover: ctx member swap; `ctx.send` id parameter; `conclude_tag` + driver expired-sweep (dispatch site 6); send-success `confirm` dispatch; `frontier_strategy` `run/run_some/process` rewrite (gate breaks return bounded `throttle_wait` wakes — never a past `deadline()`; pool-wide vs head-specific probe-failure split; busy → bounded wake with the dead acquire branches debug_asserted unreachable; empty feeds; id routing; straggler classify; conclusion forwarders; unconditional classification notify); delete `frontier_scan_index.{hpp,cpp}` and its old tests in `core_test/bootstrap_frontier_scan.cpp` (including the `sleep_for(500ms)` cooldown test). Biggest commit, but commits 1–5 pre-landed every leaf it composes; reviewers diff round semantics side-by-side against `bootstrap-coro:frontier_scan.cpp`. Verify: full suite both backends; `bootstrap.frontier_scan*` system tests must pass unmodified.
7. **"Report frontier round progress in stats and container info"** — per-head progress reporting, settlement debug log, container_info polish; one new fanout system test (see Test plan: serving-side assertion, since with two peers quorum=4 is never reached and rounds settle via `done_partial`; the bootstrapping node's own stats cannot witness distinctness). No timing assertions, to stay out of the flaky-test family.
8. **"Tune frontier scan wakeups"** — final polish after a soak: verify the driver's wake math against the new stats (`retry`, `stall`, `busy` frequency, and the `loop_frontiers` rate as the busy-spin canary), adjust the 1 s cap / 25 ms limiter retry if profiling complains, dead-code sweep. (Per-sample deadlines and bounded gate wakes already landed in 5/6; this commit is measurement-driven tuning only and may be empty.)

## Test plan

Layered, all deterministic without coroutines; the seam is **clock injection + passive container + never starting the driver thread in unit tests**:

1. **Pure values** (`bootstrap_frontier_rounds.cpp` / extend `bootstrap_policy.cpp`): `frontier_round` — feed/trim invariant (advance never exceeds the smallest-1000 ceiling), empty votes, `empty_range` at 8, conclude trichotomy. `classify_frontiers` — outdated / pending / up-to-date against a seeded ledger, one read tx, no threads. `index_keys(frontiers_query)` position keying.
2. **Scan container** (the formerly untested middle layer): drive `next/launched/process/timeout/failure/exhaust/stall/confirm/expired` synchronously on one thread with a hand-advanced `time_point`. Required cases: distinct-peer `used` growth across consecutive intents (and that the intent's `used` is a stable value snapshot); eager-4-then-cooldown pacing; quorum advance + monotonic cursor; wrap on unanimous empty; partial conclude via exhaust; **zero-completion exhaust → round closes, fresh `used`, retry after cooldown** (the wedge regression test); **settle-with-inflight → late straggler response: entry erased, straggler stat, no feed, invariant holds** and **settle-with-inflight → straggler drained via `expired()` exactly once** (the two cases the bookkeeping model exists for); unknown-id conclusion no-op; **reset-then-confirm no-op**; duplicate-id consume-once (via tag layer test); **stall-then-drain where the draining conclusion is a *successful* response** (clears `stalled`); per-sample deadline expiry via `expired()` for both open and settled rounds; **repeated `next()` with no launch leaves `round_seq` stable** (no churn while the driver parks on an empty pool); reset; rotor round-robin fairness (deterministic order is part of the contract).
3. **Strategy/system smoke**: keep `bootstrap.frontier_scan`, `frontier_scan_pending`, `frontier_scan_cannot_prioritize` in `core_test/bootstrap.cpp` unchanged (single-peer systems advance via the exhaust-partial path, so they get *less* timing-sensitive). Commit-7 fanout test, asserted from the **serving side**: two responding peer nodes; require each peer's `bootstrap_server` frontier-response counter (`stat::type::bootstrap_server, detail::frontiers, dir::out` — bootstrap_server.cpp:183) ≥ 1 before any head advances, plus `sample ≥ 2` and `done_partial ≥ 1` on the bootstrapping node. Alternative if node plumbing fights back: two scripted `nano::transport::test_channel` instances (present on this branch), each asserted to record ≥ 1 `frontiers_query` at the same start position, answered through the public `ctx.process`.
4. Debug-only invariants active throughout: the matched-samples consistency assert (§ Sample bookkeeping rule 5); strictly-increasing cursor assert; `launched()` round_seq match assert; unreachable-acquire-status assert in the sweep.

## Phase 2: topo strategy (design resolution)

The frontier refactor is the dress rehearsal; topo instantiates the same seams with **zero engine rebuild**: a passive, clock-injected, self-settling `topo_scan` container under `ctx.mutex`; one driver thread sweeping heads with the same bounded-wake discipline; id-routed sample conclusions through the (now existing) trichotomy dispatched by `query_source::topology`; the erase-on-conclusion sample model and value-snapshot intents carried over verbatim; distinct-peer rounds via `peers.acquire (node_capabilities::topo_index, used)`; `ctx.tags` as the only registry. The id→`(head, round_seq)` sample map structurally dissolves the spear/repair same-`topo_key` collision that position-keyed accounting could not.

**Already on this branch, verified — port nothing**: wire (`asc_pull_type::blocks_random=0x4`, `topo_index=0x5`, `topo_index_payload`, `blocks_random_payload` in `nano/messages/asc_pull.hpp`), server handlers for both types (`bootstrap_server.cpp`), `node_capabilities::topo_index` handshake flag cached in `peer_pool`, `nano::topo_key` + `store.topology` view + sideband maintenance + `populate_topo_index`, the `topology_channel` block-processor fair-queue partition placeholder in ctx.

**Port from `bootstrap-coro-topo:nano/node/bootstrap/topo_scan.{hpp,cpp}`** (verified pure/engine-free, near-verbatim): `topo_round` (distinct-peer voting at a fixed `topo_key`, trim to the **lowest** `page_size` keys — mirror of frontier's truncation; adaptive quorum; 2× empty threshold), `topo_band` + `repair_band (index, count, frontier_height)` (equal tiling of `[0, frontier]`, ceiling frozen per sweep), `topo_gaps` (retain-by-key/route-by-hash, `gap_threshold` latch with drain-to-zero hysteresis, bounded `retry_batch` with eviction after `gap_retries` — so an unresolvable gap can never pin the pause), `verify_topo_entries` (strictly ascending, ≥ start, height step ≤ 1, empty page = valid tip evidence). Note: that branch predates the `traffic_type::bootstrap` rename on this branch — adjust on port. Do **not** copy structure from `topo-bootstrap-8-squashed` (per owner; its O(members) walks and chunk bookkeeping are known dead ends).

**Architecture mapping**:
- **Heads are parametrized per-head, not uniform** (the round-spec idea, expressed as plain data, no virtual interface): head 0 = **spear** (`consideration_count=4` quorum, seeded by orientation at the exact highest local `(height, hash)`, gates each round on the gap latch and `pages.size () < max_pending`; tip = two consecutive empty-consensus rounds, then poll mode re-paging every `poll_interval`); heads 1..R = **repair** (`consideration_count=1`, first-reply settle, per-sweep frozen equal bands, paced by `repair_cooldown`, idle until `spear_height ≥ repair_activation_height` **or** the spear is gap-paused, pages retired inline and untracked).
- **Two strategy threads** (precedent: `dependency_strategy`): `bootstrap_topo_scan` (driver, same sweep shape and wake discipline as frontier's) and `bootstrap_topo_processing` (page pipeline: pop settled spear pages strictly in topo order, gated by the gap latch and `wait_block_processor (strategy::topology, block_processor_threshold)`; ledger redundancy filter off-mutex in its own read tx, like `process_frontiers`; fetch missing hashes via `blocks_random` mini-rounds — distinct peers per retry, ≤ `fetch_attempts`, then drop; submit arrived blocks in key order via `topology_channel`; run the `topo_gaps` retry cadence). The bounded tracked window (oldest-evict) absorbs the processor's silent overflow drops.
- **Engine extensions topo needs that frontier did not** (all additive): `query_type::{topo_index, blocks_random}`, `query_source::topology`, `strategy::topology`, `head_index_t`, stat mappers in `common.{hpp,cpp}`; `topo_index_query`/`blocks_random_query` alternatives in `query_descriptor` + `build_message`/`index_keys` (topo: `hash = start.hash`; blocks_random: first hash)/`to_query_type`; `payload_verifier` acceptance of `topo_index_payload` (replacing the stub at bootstrap_context.cpp:487-489) and of `blocks_payload` for `blocks_random` tags, with `ctx.process (blocks_payload, tag)` routing on `tag.type ()` to the topo strategy instead of `submit_blocks`; `conclude` gains the `query_source::topology` routing case; `block_processor_channel` gains the `strategy::topology` case (cpp:259-272 currently release_asserts); a `topology_limiter` (`topology_rate_limit` 500); **verdict routing**: `ctx.inspect` forwards bootstrap-source `(status, hash)` verdicts to the topo container (tracked window + gaps) under the same mutex; **orientation**: extend `store::ledger::topology_view::latest ()` (`nano/store/ledger/topology.hpp:23`, currently `optional<uint64_t>` height-only) to return `optional<nano::topo_key>`.
- **Config**: new `topo_scan_config` in `bootstrap_config.hpp` (`head_parallelism≈8`, `consideration_count=4`, `repair_consideration_count=1`, `page_size/candidates=1000`, `cooldown=3s`, `repair_cooldown=500ms`, `max_pending=16`, `fetch_batch_size=128`, `fetch_attempts=4`, `gap_threshold=1000`, `gap_retries=16`, `retry_interval=5s`, `repair_activation_height=100`, `poll_interval=15s/1s-dev`) + `enable_topology_scan` (default true — capability gating makes it inert against non-capable peers) + `topology_rate_limit`. All thresholds are starting points from the proven prototypes, re-evaluated on soak; the spec's §6.6 numbers are explicitly non-binding (`topo-index-spec.md` is conceptual background only).

**Commit outline** (design resolution): ① enums + query alternatives + `verify_topo_entries` + policy tests; ② port `topo_round`/`topo_band`/`topo_gaps` + unit tests; ③ `topology_view::latest () → optional<topo_key>` + test; ④ passive `topo_scan` container (heads, pages, gaps, tracked window; same sample-bookkeeping rules as `frontier_scan`) + deterministic synchronous tests (mirror the clean-slate 27-test precedent); ⑤ `topo_strategy` two threads + ctx routing (payload verifier, blocks_random dispatch, conclusion trichotomy, limiter, processor partition) + config; ⑥ verdict routing + gap-latch/system tests against the existing server (`core_test/bootstrap_server.cpp` topo tests already on this branch); ⑦ orientation seeding + poll mode + soak-driven tuning.

## Risks & open questions

1. **Conclusion-dispatch discipline** — any *future* tag-erase path that forgets its `conclude()` call desyncs the scan. Mitigation: `conclude`/`conclude_tag` as the single choke point (six enumerated sites; the rule, not the list, is normative); the scan's per-sample deadlines cover open-round *and* straggler entries, so any leak self-heals within one timeout; the matched-samples debug_assert; deterministic tests for every conclusion path.
2. **Cutover commit size** (~600–800 lines). Mitigation: commits 1–5 pre-land every leaf; review by side-by-side diff against `bootstrap-coro:frontier_scan.cpp` semantics; system tests unchanged.
3. **Driver unlock seam** — the world may change while the lock is dropped around a send. Mitigation: `sample_intent` is a value snapshot (no lifetime hazard by construction); the sweep discards it and re-fetches via `next()` after every relock; `round_seq` assert in `launched()`; covered by the container tests.
4. **`ctx.mutex` contention** — round merge stays on the message-processor thread (O(1000·log) at 15/s, negligible; topo adds ~more). Escalation path if profiling disagrees: deposit-only response handling (correlate + queue on the message thread, verify + merge on the driver) — a localized change the id-routed design permits without restructuring.
5. **Single-peer behavioral change** — exhaust concludes partial on the first reply, weakening redundancy exactly when diversity is lowest (same call the coro branch made; faster than today). Watch `done_partial` vs `done` on dev/live soak.
6. **Busy-handling coarseness** — `busy` is exclusion-list-relative, but the sweep breaks pool-wide with a 100 ms bounded wake instead of per-head busy bookkeeping, and the limiter token spent before a busy result is accepted as lost. Both are deliberate simplicity-over-throughput calls in degraded-peer scenarios; watch the `busy`-adjacent stats and `loop_frontiers` rate on soak before adding machinery.
7. **Request economics** — keep `frontier_rate_limit=15`; each advance costs ~4 requests, same as today. Re-evaluate with the new `done/done_partial/retry` stats after landing; only then consider raising.
8. **Stragglers still classify** (data is paid for). Flip to drop if priority-set churn appears in soak stats.
9. **Topo spear/repair interplay under one driver** — whether repair cadence starves spear launches under the single scan thread, and whether the page pipeline should share or own its worker. Decided during phase 2 detail design, informed by phase 1's driver behavior; the per-head parametrization keeps both options open.
10. **Unify `frontier_round`/`topo_round` into a template?** Deferred — two concrete types first; unify only if a third consumer appears.

## Explicit non-goals

- No coroutines, strands, awaitables, or virtual-clock test plumbing.
- No generic round-engine framework, no virtual `round_policy` interface, no second in-flight registry — `ctx.tags` stays the single correlation map.
- No changes to priority/database/dependency strategies, `account_sets_index`, `peer_pool` internals, peer scoring/penalty policy, or the block processor.
- No new config keys in phase 1; no retuning of existing defaults (`throttle_wait` is reused, not changed).
- No wire/server changes in either phase (the topo protocol and server are already complete on this branch).
- No migration of other strategies onto the rounds machinery (the door stays open; out of scope).
- No topo implementation in phase 1 — phase 2 starts only after the frontier refactor has landed and been evaluated.
