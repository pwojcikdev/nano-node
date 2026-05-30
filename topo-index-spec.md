# Topology-Bootstrap Subsystem — Implementation-Independent Specification

> **Subject.** The topology-bootstrap subsystem of nano-node: the in-memory engine that
> discovers blocks in topological order from peers, fetches them, submits them to the block
> processor in dependency order, and reports progress. Today this is `topo_scan_index` +
> `topo_strategy`; this spec deliberately does **not** assume that structure.
>
> **Status.** Requirements + constraints extracted from the existing implementation and its
> iteration history, to drive a clean-slate redesign. The existing code is treated only as
> *evidence of behavior, constraints, and edge cases discovered during development*. No
> architecture, class layout, naming, or control flow is preserved unless §6 gives a
> behavioral reason.
>
> **Companion doc.** `topo-index-redesign-problem.md` (the failure analysis that motivated this).

---

## 1. Purpose and scope

### 1.1 The problem it solves
The node must sync a ledger that is a **DAG of per-account block chains** (live network ≈ 207M
blocks). "Topological order" is a total order on blocks by `topo_key = (topo_height, hash)`, where
`topo_height = 1 + max(topo_height of dependencies)` and `hash` breaks ties. Discovering blocks in
this order means **every dependency is offered before its dependents**, which lets the block
processor accept a long run of blocks without gaps.

A peer exposes a **topo-index**: a height-ordered enumeration of its ledger. The subsystem pages
through peers' topo-indices, learns which block hashes exist and in what order, fetches those
blocks, and feeds them to the processor in order. It is one of five concurrent bootstrap
strategies (`priority`, `database`, `dependency`, `frontier`, `topology`); it is the
order-aware bulk-sync strategy.

### 1.2 Responsibilities
- Discover block hashes in topological order across many peers, robustly against individual
  peers that are slow, lagging, incomplete, or hostile.
- Decide which hashes to fetch next, fetch them, and avoid fetching anything already local.
- Hand fetched blocks to the processor **in an order that respects dependencies** at the
  discovery frontier.
- Consume processor results (accepted / dependency-missing / terminally-bad) and react.
- Track and report monotonic sync progress; detect when the whole topology is synced; resume
  cheaply after a restart; poll for newly-appended blocks once caught up.
- Do all of this with **per-operation cost independent of how much is outstanding**, and without
  starving the other four strategies or inbound message processing.

### 1.3 Explicitly out of scope
- **Account-level dependency resolution / by-hash dependency chasing.** If a dependency is
  missing, the subsystem does not walk accounts or chase predecessors itself — that is the
  `priority`/`dependency` strategies' job. Topo repair is *pure re-enumeration* of the index.
- **Block validation / consensus / cementing.** The block processor and ledger own that. The
  subsystem only observes the processor's verdict per block.
- **The wire protocol, channel/peer management, the request tag pool, rate limiting, and the
  block processor.** These are fixed external services the subsystem consumes (see §3.3, §4.3).
- **Guaranteeing completeness of historical holes.** Filling every old gap is best-effort;
  anything still missing at quiesce is left to a re-bootstrap and the other strategies (§5,
  E18/E19; §6 I8).

---

## 2. Externally-observable behavior

The "users" of this subsystem are (a) the rest of the bootstrap context and the block processor,
and (b) the operator (via config, logs, and stats). There is no human in the per-block loop.

### 2.1 Inputs
1. **Local ledger topo-index** (read-only): the highest `topo_key` we already hold
   (`topology.latest()`), and whether a given hash already exists (`block_exists`). Used for
   restart orientation and pre-fetch redundancy.
2. **Peer topo-index pages**: in response to a request `(start_cursor, count)`, a peer returns up
   to `count` consecutive `topo_key`s `≥ start_cursor`. An empty page signals end-of-topology.
3. **Fetched blocks**: in response to a request of up to N hashes, a peer returns the
   corresponding block objects (missing ones simply absent).
4. **Processor results**: per submitted block, exactly one verdict — *accepted* (`progress`/`old`),
   *dependency-missing* (`gap_previous`/`gap_source`/`gap_epoch_open_pending`), or *terminal*
   (fork / bad signature / unreceivable / …).
5. **Reachable peers** with the topo-index capability, plus their identities (node id), supplied
   by peer scoring/channel selection.
6. **Config** (§6.6) and a shutdown signal.

### 2.2 Outputs
1. **Topo-index page requests** to chosen peers (a cursor + count), each steered to a *distinct*
   peer for the current quorum round.
2. **Block fetch requests** (lists of hashes), only for hashes not already local.
3. **Blocks submitted to the processor**, tagged as topology-sourced so results route back here,
   delivered **in dependency-respecting order at the frontier**.
4. **A monotonic progress cursor** (top of the contiguous in-ledger prefix) for telemetry.
5. **Parking/idle signals** that let the driving threads sleep when there is nothing to do, and a
   **caught-up signal** when the whole topology is synced.
6. **Stats and a container-info snapshot** for observability.

### 2.3 Interaction flow (steady state)
1. **Orient** once at startup: seed the discovery frontier at the highest local `topo_key`.
2. **Discover**: the spear pages the frontier forward; repair heads re-page bands below it. Each
   page is finalized only after a distinct-peer quorum agrees.
3. **Fetch**: drain discovered-but-unfetched hashes lowest-key-first, skipping any already local.
4. **Submit**: hand received blocks to the processor — contiguously in topo order at the frontier,
   freely (un-gated) for historical backfill.
5. **React**: on each result, mark the block done / retain-and-retry (dependency gap) / evict
   (terminal); advance the progress cursor over the contiguous done prefix.
6. **Quiesce**: when the spear reaches the tip and nothing is outstanding, declare caught-up;
   periodically re-poll for newly-appended blocks.

### 2.4 Success criteria
- A freshly-started, empty node syncs the entire reachable topology and reaches **caught-up**.
- A restarted node **resumes near its existing data**, not from genesis, and converges.
- Throughput does **not** degrade over a multi-hour run; one core does not peg at 100%; the other
  strategies and inbound message processing keep running at full rate throughout.
- No discovered key that the **frontier** needs is ever silently dropped (§6 I8).

---

## 3. Core capabilities

### 3.1 Multi-head topological discovery
Several independent scanning **heads** walk the topo-index concurrently; the count is configurable
(default 8 = 1 spear + 7 repair). The user explicitly wants the same *range behavior* as today but
**more heads**.

- **Spear (the lead head)** — scans the highest-known region *forward* in topo order, extending the
  discovered frontier. It is the bulk of forward progress.
- **Repair heads** — continuously re-sweep the region *below* the frontier (upward, in normal topo
  order) to re-enumerate keys the spear's quorum may have skipped, and to surface historical holes.
  The R repair heads divide `[0, frontier]` into **R equal bands** that tile the range with no
  overlap: head `i` (0-based among repair heads) sweeps `[i·F/R, (i+1)·F/R]` and restarts at its
  floor on reaching its ceiling. Because each band is `1/R` of the range, the full range is
  re-covered ~R× faster than a single sweep, and head 0's band reaching down to genesis guarantees
  every key is eventually re-enumerated. (See §5 E7–E9 for the frozen-ceiling / equal-band / upward
  requirements that this shape encodes.)

**Decisions a head makes:** whether it is eligible to query now (cooldown, backpressure, gap-pause,
activation gate, distinct-peer availability); what cursor to query; when a page is final (quorum
reached); when its sweep wraps (and re-snapshots its band); when to idle (spear at tip / repair
quiesced).

### 3.2 Distinct-peer quorum per page
A page is **not** trusted from a single peer. Each head accumulates a **union of responses from N
distinct peers** (by node id) for the same cursor before finalizing — N is larger for the spear
(it must resist a fast un-synced peer poisoning the frontier) and smaller for repair heads
(coverage also accrues across re-sweeps with different peers). The quorum size **adapts down** to
the number of reachable peers so it can always finalize (§5 E16). The spear declares **end-of-
topology only after ≥2 distinct quorum rounds agree the page is empty** (anti-poisoning, §6 I3).

### 3.3 Fetch with redundancy skip
Produce **hashes to fetch, lowest topo_key first** (dependencies before dependents), with a
bounded number in flight and timeout-driven retry. Before fetching, **filter out any hash already
in the local ledger** and mark it resolved without a network round-trip — this is the dominant case
when caught up and re-scanning. The redundancy check (a ledger read) and the actual fetch must
**not** be done while holding the bootstrap-wide lock (§6 I9).

### 3.4 Ordered submission
Produce **blocks to submit to the processor**:
- At the **frontier**, strictly **contiguous in topo order** — never submit a dependent before its
  dependency, and stop extending past the first un-fetched hole. This is what keeps gap rate low.
- For **historical backfill** below the submit frontier, submission is **un-gated** (neighbors are
  already in the pipeline/ledger; a genuine out-of-order submit is simply rejected by the processor
  as a gap and retried later).
Bound each submission batch so a single hand-off cannot overflow the processor's per-source queue.

### 3.5 Result feedback
Consume the processor's per-block verdict and route it to the right block:
- **Accepted** → mark resolved; advance the progress cursor over the now-contiguous done prefix.
- **Dependency-missing** → if at the frontier, **retain and re-submit later** (a repair head may
  fill the dependency); count it toward the gap-pause. If historical backfill, **drop it** (a later
  re-sweep re-attempts it) — do not retain or count it (§5 E3).
- **Terminal** → evict; it is not a hole (advance past it at the frontier).
Marking a batch of results must be **O(batch)**, not O(batch × outstanding) (§5 E1, §6 I10).

### 3.6 Restart orientation
Before discovery begins, **seed the frontier at the exact highest `topo_key` the local ledger holds
(height *and* hash)** so a restart resumes immediately after its last block instead of re-paging the
whole top from hash 0 (or from genesis). Anything below the seed that is locally missing is left to
the repair heads. An empty local index seeds at genesis. Seeding must keep the progress cursor
monotonic (§6 I1). Orientation can be disabled by config.

### 3.7 Termination and poll mode
- **Caught-up** = the spear has seen the tip *and* nothing remains outstanding. Repair heads must
  **quiesce** (stop re-discovering already-synced keys) once the spear is done and no frontier gap
  remains, or the outstanding set never drains to empty and caught-up is never reached (§5 E19).
- **Poll mode**: once caught-up, periodically re-arm the spear to re-page from its cursor and pick
  up blocks appended since the tip was reached.
- **Parking**: when there is no eligible work, the driving threads must sleep (on the shared
  condition variable) rather than spin.

### 3.8 Required integrations (fixed external boundaries)
- **Peer scoring / channel selection** — pick a channel, filtered by topo-index capability and by
  an exclusion list (the peers already queried this round).
- **Request tag pool** — register each in-flight request, match replies, enforce a global cap and
  per-request timeout. The subsystem must respect the global in-flight cap and a per-strategy rate
  limit before issuing a request.
- **Block processor** — submit blocks (tagged topology-sourced), respect its per-source queue
  capacity, and receive results via the shared inspect callback.
- **Ledger** — `topology.latest()`, `block_exists()` (both read-only, off the hot lock).
- **Shared bootstrap context** — one mutex + one condition variable currently guard all shared
  state (all five strategies, the tag pool, peer scoring, every strategy's index). The lock model
  is itself in scope for the redesign (§6 N4, §9 Q3).

---

## 4. State and context requirements

### 4.1 Per-run (in-memory) state
- **The member set**: for every discovered-but-not-yet-finished hash, its `topo_key`, lifecycle
  state, the fetched block payload (retained so a dependency-gapped block can be re-submitted
  without re-fetching), and last-action timestamp. This must be **routable by hash in O(1)**
  (results arrive by hash) and **orderable by topo_key** where order matters.
- **A submit frontier position** (monotonic): the top of the contiguous prefix already handed to
  the processor; partitions members into *frontier* (ordered, gated) vs *historical backfill*
  (un-gated). The partition class of a member is fixed at insert.
- **A progress cursor** (monotonic): top of the contiguous in-ledger prefix; report-only.
- **Per-head scan state**: cursor, the in-progress quorum union, distinct peers queried this round,
  the frozen quorum size, and (repair) the frozen band `[floor, ceiling]`.
- **A frontier gap count** and a **pause latch** (hysteresis).
- **O(1) per-state counters** for backpressure and observability.
- Time-ordered structures for **fetch-retry** and **gap-retry** so timeouts are processed without a
  full scan (§5 E1/E6-of-problem-doc; §6 I11).

### 4.2 Cross-run persistence
- **None of its own.** The only persistent input is the **local ledger's topo-index**, read once at
  orientation (and implicitly via redundancy checks). All in-memory state is rebuilt each run; a
  restart re-orients and re-converges. The design must assume it can be killed and restarted at any
  point without corrupting progress (the ledger is the source of truth).

### 4.3 What must never be assumed
- **Peer honesty or completeness.** A page may be empty, short, stale, internally inconsistent
  (height jump > 1), or maliciously crafted. A fetched-block reply may omit hashes. Never finalize
  the frontier or declare the tip on a single peer's word (§3.2, §6 I3, §5 E6/E20).
- **Delivery order or timeliness.** Replies can arrive late, out of order, after the cursor moved,
  or after the member already changed state. Stale/tardy inputs must be **detected and dropped**,
  never allowed to backtrack state (§5 E15).
- **That a discovered hole will be filled by us.** Frontier gaps block caught-up until filled;
  historical holes are best-effort (§1.3, §6 I8).
- **That outstanding work is small.** Designs must hold at the 207M-block scale with a bounded
  active set; the cost of any per-block/per-batch operation must not grow with outstanding count.
- **A monotonic or single reachable peer set.** Peer count fluctuates; the quorum must finalize
  with however many distinct peers exist *now*.

---

## 5. Edge cases and gotchas (discovered during development)

Each is a real failure mode the previous implementation(s) hit. The redesign must structurally
prevent it, not patch it.

**E1 — O(outstanding) hot paths under the shared lock (the headline bug).**
Every "find the next actionable member" query scanned a single ordered container from the front,
filtering by state, and the watermark-advance re-walked all skipped below-frontier members from the
beginning *every call*. Under sustained operation this became O(outstanding) per call, amplified up
to ~128× per fetch batch, all under the node-wide bootstrap mutex. One core pegged at 100%, and
because the lock is shared with the other four strategies + inbound message processing + result
inspection, the **whole node stalled** — and the stall was self-sustaining (the starved result path
couldn't drain members, so outstanding stayed pinned at its cap, so the scan stayed long).
*Why it mattered:* total bootstrap halt, no self-recovery. *Requirement:* **every per-block /
per-batch operation O(1) / O(log n) amortized / O(work produced)** — never O(members), O(gaps), or
O(below-frontier holes) (§6 N1). Structural work must be **per-batch, not per-element** (§6 I10).

**E2 — Below-frontier re-discoveries piling at the scan front.**
Repair heads continuously re-enumerate keys below the frontier. The old design re-injected them at
the front of the one ordered container, where every "next" scan re-skipped them forever.
*Requirement:* re-discoveries below the submit frontier are a **separate region** kept entirely out
of the frontier ordering/prune/submit walks; resolving one **erases it immediately** rather than
relying on a front scan to reach it.

**E3 — Historical gaps accumulating and stalling the spear.**
If a re-discovered historical block is submitted, gaps (its own dependency is also a hole), and is
then *retained + counted + retried*, thousands pile up: they inflate the outstanding count, trip the
spear's backpressure, and churn forever on the retry timer. *Requirement:* a **backfill gap is
dropped, not retained or counted** — a later re-sweep re-attempts it once its dependency may be
filled. **Only frontier gaps** are retained, counted, and drive the pause; they are bounded by the
gap threshold regardless of how many historical holes a sweep turns up.

**E4 — Resolved-but-unpruned members inflating backpressure.**
If the discovery backpressure metric counts members that are already in-ledger but awaiting prune, a
few gaps that wedge the prune pin the count at its cap and stall the spear permanently.
*Requirement:* the backpressure metric counts **only genuinely-active** members (pending / in-flight
/ received / submitted / gapped), **excluding** resolved-awaiting-prune ones. Gap-driven pausing is
the dedicated pause latch's job, nothing else's.

**E5 — Cascading gaps from submitting past a hole.**
Submitting the dependent of a missing block makes the dependent gap too, which cascades. Ignoring
gaps without bound just manufactures more gaps. *Requirement:* a **bounded gap threshold**; below
it the spear keeps discovering and submitting past gaps (progress on independent chains), at it the
spear **pauses** and submission **stops extending past the lowest gap**, with **hysteresis** — resume
only when the frontier gap count returns to **zero** (`HALVE THE CASCADE` / `DO NOT STALL`).

**E6 — Frontier poisoned / tip declared by one fast un-synced peer.**
A single fast peer returning an empty or short page could prematurely declare the tip or let the
frontier skip keys. *Requirement:* **distinct-peer quorum** per page; tip only after **≥2 distinct
quorum rounds** agree empty (`DISTINCT PEERS`; §6 I3).

**E7 — Repair head chasing a live, moving frontier forever.**
If a repair band tracks the *live* frontier, a fast spear keeps dragging the ceiling away and the
head never completes a sweep, so the keys in its band are never re-enumerated. *Requirement:*
**freeze each band's ceiling per sweep** (snapshot at wrap; hold it fixed until the next wrap).

**E8 — Downward repair sweep (reverted twice).**
Repair heads were tried scanning *downward*; this was reverted (`REVERT DIRECTIONAL TOPO`,
`REVERSE REPAIR HEADS`). Sweeping **upward** (dependencies before dependents) lets the fetch/submit
pipeline drain what a repair head discovers, which keeps the member set from piling up.
*Requirement:* repair sweeps go **upward in topo order**.

**E9 — Geometric band split → equal bands.**
An earlier split gave head `h` the range `[ceiling/2^(h-1), ceiling]` (geometric), over-covering the
top and starving the bottom. Replaced (`REPAIR HEADS EQUAL RANGES`) with **equal bands tiling
`[0, F]`**, so coverage is uniform and each band is swept R× faster than a full sweep. When the
frontier is smaller than the head count, every head just re-scans the whole tiny range.

**E10/E11 — Restart re-paging from genesis / re-paging the same height from hash 0.**
Without orientation a restart re-discovers everything it already has — hours wasted. Seeding at only
the height (not height+hash) still re-pages same-height blocks. Earlier orientation used a brittle
peer-probe binary search, since removed. *Requirement:* seed at the **exact highest local
`(height, hash)`**; repair backfills anything missing below (`ORIENT FROM LATEST`, `ORIENT FIX`).

**E12 — Thrashing the dense, dependency-free low layer.**
At the start, the frontier sits at `topo_height ≈ 1` for a long time (genesis + very many epoch-open
blocks, all dependency-free). Repair heads re-scanning that layer only thrash, and those roots can
never gap. *Requirement:* repair heads stay **idle until the frontier passes an activation height**
(`DENSITY CHECK`).

**E13 — Progress cursor dragged backward by a below-cursor fill.**
A repair head filling a dependency below the reported position must not move the cursor back.
*Requirement:* the progress cursor is **monotonic** (§6 I1); backfill fills below it don't touch it.

**E14 — Progress cursor outrunning the submit frontier on a redundancy hit.**
A member resolved without ever being submitted (pre-fetch redundancy) can advance the progress
cursor past the submit frontier. *Requirement:* keep the **submit frontier ≥ progress cursor** so
the forward submit walk skips the pruned region in one step and never strands a frontier member
below the walk's start.

**E15 — Stale / tardy inputs backtracking state.**
A page whose start ≠ the head's current cursor must be **dropped**. A block delivered after its
member left the fetch states must **not** move it backward. Time-ordered retry structures must
**lazily validate** their fronts (skip entries whose member already moved on). *Requirement:* every
inbound path is idempotent against staleness and never regresses a member's lifecycle.

**E16 — Quorum that can never be filled.**
If the quorum target exceeds the number of reachable distinct peers, the gather waits forever.
*Requirement:* **freeze the target to min(desired, reachable)** and **cap it down to the peers
actually reached** when the distinct pool is exhausted, so the gather always finalizes (`DO NOT
STALL`).

**E17 — Spear burning its budget on a transient peer outage.**
If no topo-capable peer is reachable, the spear must not commit a head and slide into cooldown over
a failure that isn't its own. *Requirement:* check **capable-peer availability before committing a
head**, surfacing the condition as a channel-wait, not as "no work."

**E18 — Silently losing a key the frontier needs.**
*Requirement:* a frontier dependency the spear's quorum skipped must be **retained** (as a frontier
gap) and keep caught-up false until a repair head fills it — **never** silently dropped. Historical
holes are explicitly best-effort.

**E19 — Never reaching caught-up because repair never stops.**
If repair keeps re-discovering already-synced keys, the outstanding set never empties.
*Requirement:* repair heads **quiesce** once the spear is done and no frontier gap remains.

**E20 — Malicious / broken page accepted.**
A page with a height jump > 1, entries not strictly ascending, or a first entry below the requested
cursor indicates a hole in the peer's index (broken / incomplete / malicious). *Requirement:*
**reject** such a page wholesale (do not partially ingest it).

---

## 6. Invariants and constraints

### 6.1 Ordering invariants
- **I1 — Progress-cursor monotonicity.** The reported progress cursor never decreases, including
  across orientation seeding and below-cursor repair fills.
- **I2 — Submit-frontier monotonicity & contiguity.** The submit frontier only advances; at the
  frontier, a block is never submitted before all its (frontier) dependencies, and the walk stops at
  the first un-fetched hole.
- **I3 — Distinct-peer quorum.** A spear page finalizes only after a union of `consideration_count`
  *distinct* peers; the tip is declared only after ≥2 distinct rounds of empty pages. Repair pages
  finalize on a smaller distinct quorum, with coverage accruing across re-sweeps.
- **I4 — Submit-frontier ≥ progress cursor.** Always (E14).

### 6.2 Validation rules
- **I5 — Page well-formedness.** Reject any page that is not strictly ascending, starts below the
  requested cursor, or steps more than one in `topo_height` between consecutive entries (E20).
- **I6 — Lifecycle legality.** A member advances pending → in-flight → received → submitted →
  {in-ledger | gapped | terminal}; a gapped member may return to submitted on retry. Inbound events
  that would illegally regress a member are ignored (E15).
- **I7 — Region stability.** A member's region (frontier vs backfill) is decided once at insert and
  never changes; results route by the member's stored region.

### 6.3 Coverage / safety invariants
- **I8 — No lost frontier key.** Until the tip is reached, a frontier gap keeps the subsystem from
  declaring done; the frontier never silently drops a key it needs. (Historical holes are
  best-effort and may be left to a re-bootstrap.)
- **I9 — Redundancy pre-filter.** Never fetch a block already in the local ledger; mark it resolved
  instead. Ledger reads run **outside** the shared lock.
- **I10 — Repair coverage guarantee.** The repair bands tile `[0, frontier]` with no gap, so every
  key the spear's union skipped is eventually re-enumerated; frozen ceilings prevent a fast spear
  from making a band un-sweepable (E7).

### 6.4 Performance invariants (the actual point of the redesign — non-negotiable)
- **N1 — Scale-independent hot paths.** Every per-block / per-batch operation (next-to-fetch,
  next-to-submit, record-result, cursor-advance/prune, retry reclaim) is **O(1) / O(log n)
  amortized / O(work produced)**. None may be O(total members), O(below-frontier members), or
  O(gaps). Cost must not grow as outstanding members or gaps grow.
- **N2 — Per-batch structural work.** Marking a batch of redundant or processed results triggers at
  most O(batch) structural work — never a full structural pass per element.
- **N3 — Bounded, predictable memory.** The active member set is bounded (today by the discovery
  backpressure cap, default 40k) with a stated eviction/erase-on-resolve policy; historical backlog
  is kept out of the hot paths and likewise bounded. State the bound and the policy in the design.

### 6.5 Concurrency / liveness constraints
- **N4 — No node-wide starvation (lock model: DECIDED).** All shared state stays under the existing
  bootstrap-context mutex (`ctx.mutex`); **no new mutex is introduced**, and the subsystem remains
  caller-synchronized (not internally locked). This is approach (c) from the problem doc §7.4.
  Consequently, starvation is prevented **solely** by N1: every critical section taken under
  `ctx.mutex` must be provably **O(1)/O(log n)** — there is no per-subsystem lock to decouple a slow
  section, so a *single* O(members) stretch here stalls the entire node (all five strategies +
  inbound message processing + result inspection share this one lock). Long work — ledger redundancy
  reads and block-processor submits — must run **outside** `ctx.mutex`: drain a bounded batch under
  the lock, release it, then do the I/O. The design must state the **maximum critical-section cost**
  for each hot path and show it is height-/member-independent.
- **N5 — No self-sustaining stall.** Result-resolution throughput must never depend on a lock whose
  hold time depends on the number of unresolved members.
- **N6 — Progress under partial peer availability.** With at least one capable peer, the spear makes
  progress; quorums finalize against the reachable pool; transient peer outages park, not spin.

### 6.6 Config knobs (current defaults — the redesign may re-interpret, but should preserve the
levers)
`head_count`=8 (1 spear + 7 repair), `spear_weight`=4 (spear:repair scheduling ratio),
`consideration_count`=4 (spear quorum), `repair_consideration_count`=2 (repair quorum),
`candidates`=1000 (kept per finalized page / page size), `cooldown`=3s (per-cursor re-query),
`block_retry`=5s (fetch + gap retry interval), `block_batch_size`=128 (fetch batch),
`max_blocks_outstanding`=10k (in-flight fetch cap), `max_blocks_queued`=40k (discovery
backpressure cap), `block_processor_threshold`=2000 (submit batch / processor capacity gate),
`gap_threshold`=1000 (frontier gaps tolerated before pause; clamp ≥1),
`repair_activation_height`=100, `enable_orient`=true. Context-level: `max_requests`=1000 (global
in-flight cap), `request_timeout`=15s, `channel_limit`=16/peer, `topology_rate_limit`=500.

---

## 7. Error handling expectations

| Failure mode | Detection | Response |
|---|---|---|
| **Peer page stale** (start ≠ cursor) | compare to head cursor | Drop the page; no state change. |
| **Peer page malformed** (E20) | well-formedness check (I5) | Reject wholesale; count an invalid-verify stat; do not penalize the head's progress (re-query with a different peer next round). |
| **Peer page empty (spear)** | empty union | Not the tip yet — needs ≥2 distinct empty rounds; otherwise re-query at the same cursor on cooldown. |
| **Peer page empty (repair)** | empty union | No progress; re-query the same band cursor with fresh peers on cooldown. |
| **No capable peer reachable** (E17) | capability count = 0 | Park as channel-wait; do not commit a head into cooldown. |
| **Distinct pool exhausted** (E16) | queried ≥ reachable, round not full | Cap the quorum to peers reached so it finalizes. |
| **Fetch reply missing a hash** | hash absent from reply | Member stays in-flight; retry on timeout. |
| **Fetch timeout** | in-flight older than `block_retry` | Reclaim to pending for re-fetch (time-ordered, O(reclaimed)). |
| **Submit → dependency gap (frontier)** | processor verdict | Retain + re-submit after retry; count toward gap-pause; at threshold, pause spear. |
| **Submit → dependency gap (backfill)** | processor verdict + region | **Drop** (E3); a later re-sweep re-attempts. |
| **Submit → terminal** (fork/bad-sig) | processor verdict | Evict; log loud; advance past it (not a hole). |
| **Backpressure (active window full)** | outstanding ≥ cap | Pause spear discovery (repair/fetch/submit continue to drain). |
| **Processor queue full** | capacity gate | Wait for capacity before submitting; never overshoot the per-source cap. |
| **Shutdown** | stop flag | All threads observe the flag under the condition variable and exit promptly. |

General principles: **drop and re-derive** rather than trust questionable input; **retry on
timers**, not by re-scanning; **park** when idle; **never** let one peer's behavior finalize the
frontier or the tip; **fail best-effort** on historical holes (leave them to re-bootstrap / other
strategies) but **never** on a frontier key (I8).

---

## 8. Test scenarios

### 8.1 Happy paths
- **H1 — Cold sync.** Empty ledger; one or more honest peers with the full topology. Expect: spear
  walks from genesis to tip, blocks fetched and submitted in order, near-zero gaps, caught-up
  reached, member set drains to empty.
- **H2 — Warm restart.** Ledger holds a large prefix. Orientation seeds at the exact highest local
  `(height, hash)`; the spear resumes just above it; the progress cursor starts there and only
  advances; the genesis layer is **not** re-paged. Repair fills any local holes below the seed.
- **H3 — Caught-up + poll.** After caught-up, new blocks are appended at peers; poll mode re-arms
  the spear and ingests them.

### 8.2 Edge-case scenarios
- **C1 — Sparse / lagging peers.** Reachable distinct peers < quorum target. Expect: quorum caps to
  the pool and still finalizes; no stall.
- **C2 — Frontier gap below threshold.** A handful of frontier dependencies missing. Expect: spear
  keeps progressing on independent chains; repair fills them; gap count returns to 0.
- **C3 — Frontier gaps reach threshold.** Expect: spear pauses; submission stops past the lowest
  gap; repair drains gaps to 0; spear resumes (hysteresis verified — it does **not** resume at
  threshold−1).
- **C4 — Many historical holes.** A re-sweep surfaces thousands of backfill holes that gap. Expect:
  they are dropped, not retained; outstanding count stays bounded; the spear is **not** stalled
  (E3/E4 regression).
- **C5 — Backpressure.** Discovery outruns fetch/submit. Expect: spear pauses at the cap, fetch and
  submit drain, spear resumes — and resolved-awaiting-prune members do **not** keep it paused.
- **C6 — Repair activation gate.** Frontier below activation height. Expect: repair heads idle (no
  thrash on the dense low layer); they activate once the frontier passes it.
- **C7 — Band tiling.** With R repair heads and frontier F, verify the bands tile `[0, F]` with no
  gap/overlap, ceilings are frozen per sweep, and every key is eventually re-enumerated (I10).

### 8.3 Regression tests (one per discovered bug in §5)
- **R-E1 — Scale independence.** Drive the index to its member cap with a large below-frontier
  backlog and many gaps, then call each hot path repeatedly; assert **bounded, flat** per-call cost
  (e.g. instrument touched-element counts) independent of member count. This is the central
  regression and the reason for the redesign.
- **R-E2/E3 — Backlog isolation.** Re-inject many below-frontier keys (some that gap); assert they
  never appear in the frontier prune/submit walks and never grow the outstanding count.
- **R-E5 — Cascade cap + hysteresis.** Inject a frontier hole; assert dependents are not submitted
  past it once paused, and resume only at zero gaps.
- **R-E7/E8/E9 — Repair shape.** Assert frozen ceilings (a moving spear doesn't change a head's
  active band mid-sweep), upward direction, equal bands.
- **R-E10/E11 — Orientation.** Seed at exact `(height, hash)`; assert no re-page of the existing
  prefix and a monotonic cursor.
- **R-E13/E14 — Cursor monotonicity & ≥ submit frontier.** Resolve a below-cursor member and a
  redundancy hit; assert the cursor never regresses and the submit frontier never falls below it.
- **R-E15 — Staleness.** Deliver a stale page and a tardy block; assert no state change / no
  regression.
- **R-E16/E17 — Liveness.** Quorum > reachable peers, and a zero-capable-peer window; assert finalize
  / park rather than stall.
- **R-E19 — Quiesce.** After the spear is done with no frontier gap, assert repair idles and members
  drains to empty so caught-up is reached.
- **R-N4 — Lock hold.** Under the chosen lock model, assert the maximum critical-section cost is
  O(1)/O(log n) (e.g. a stress test that confirms the other strategies' request rate does not
  collapse while topo is saturated).

### 8.4 Ambiguous / adversarial inputs
- **A1 — Malicious empty/short peer** trying to declare the tip early (defeated by the ≥2-distinct-
  round rule).
- **A2 — Malicious page** with a height jump > 1 or non-ascending entries (rejected by I5).
- **A3 — Peer that floods unknown keys** below the frontier (dedup against the member set; bounded
  by backpressure; classified as backfill; cannot stall the spear).
- **A4 — Peer that never returns the gapped dependency** (frontier gap retained → caught-up stays
  false; historical gap dropped → best-effort, left to other strategies).
- **A5 — Duplicate / replayed page from the same peer** (distinct-peer dedup means it does not count
  toward the quorum).

> The existing implementation documents that the index is **drivable without the strategy** (the
> heads, quorum, and member lifecycle can be exercised by direct calls with an injectable clock).
> The redesign should preserve a similarly **testable-in-isolation** core so the above can be unit
> tested deterministically without real peers or the processor.

---

## 9. Open questions (need confirmation before designing)

- **Q1 — In-memory member set vs. harder bounding.** Is an in-memory member set (bounded ~40k
  active) the right model at 207M-block scale, or should discovered-but-unfetched keys be bounded
  harder / spilled / re-derived from the index on demand? (Problem-doc §10.)
- **Q2 — Suppress below-confirmed re-discovery?** Should repair re-discovery below a *confirmed*
  region be suppressed so the index stops re-injecting members that are immediately pruned, rather
  than relying on dedup + immediate-erase? Trade-off: less churn vs. weaker coverage of late-
  arriving holes.
- **Q3 — Lock model. RESOLVED (2026-05-30):** stay on the shared `ctx.mutex`; introduce **no new
  mutexes** (the subsystem stays caller-synchronized). The full burden of N4 therefore shifts onto
  N1 — every critical section must be provably O(1)/O(log n), with all I/O kept outside the lock.
  See §6.5 N4.
- **Q4 — Head count & weighting at scale.** The user wants *more* repair heads. Is 8 the target, or
  should head count scale with peer count / be tuned empirically? Does `spear_weight` stay 4:1?
- **Q5 — Backfill completeness contract.** Confirm that historical holes below the seed are
  acceptably *best-effort* (left to re-bootstrap + other strategies), i.e. the topo subsystem does
  **not** guarantee filling every old gap — only that it never abandons a *frontier* key.
- **Q6 — Poll cadence & re-orientation.** Is the current poll interval (15s prod / 1s dev) and
  "re-arm spear from cursor" sufficient, or should poll re-read `topology.latest()` to detect
  externally-added blocks?
- **Q7 — Consumers of "one ordered structure."** Do any external consumers (container-info, stats,
  RPC, the `cursor()` report) rely on a particular internal structure or on more than the monotonic
  cursor + counters this spec exposes? If so, list them so the design preserves the contract.
- **Q8 — Capability gating & protocol.** Is the topo-index wire protocol (`topo_index` /
  `blocks_random` pull types, 1000-entry pages, 128-hash fetches, capability flag) fixed for this
  redesign, or also open to change? This spec assumes it is **fixed**.

---

### Appendix — Glossary
- **topo_key** = `(topo_height, hash)`, lexicographic. **topo_height** = `1 + max(deps)`; genesis &
  epoch-opens ≈ 1.
- **Spear** = lead head extending the frontier. **Repair head** = background re-sweeper of a band
  below the frontier. **Frontier** = region above the submit cursor (ordered, gated). **Backfill /
  backlog** = region at/below the submit cursor (un-gated re-discoveries).
- **Quorum / consideration** = distinct-peer union required to finalize a page.
- **Gap** = a submitted block the processor rejected for a missing dependency. **Terminal** =
  fork / bad-signature / unusable. **Redundant** = already in the local ledger.
- **Caught-up** = tip seen + nothing outstanding. **Poll mode** = post-caught-up re-paging.
