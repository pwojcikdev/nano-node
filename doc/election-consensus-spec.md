# Nano Election Consensus — Clean-Room Specification

Status: descriptive specification of the consensus rules **as currently implemented** in
this repository's `nano::election` and its immediate collaborators. It is written so that an
independent team can re-implement the same behavior without reading the C++ source. It
describes *rules and invariants*, not data structures or class layout.

Scope: the decision procedure by which a node decides that exactly one block at a given
account chain position is irreversibly confirmed. Out of scope: block validation rules,
ledger/cementing internals, wire serialization, bootstrap.

Network constants below use **live-network** values. Where the development network differs
materially it is given as `(dev: …)`. All time/percentage values are configurable unless
stated otherwise; defaults are quoted.

---

## 1. Terms and quantities

- **Account chain**: a per-account singly-linked list of blocks. Each block has a
  `previous` (empty for the first/open block).
- **Root**: the chain position a block occupies. For a non-open block the root is its
  `previous` hash; for an open block it is the account. Two blocks **conflict** iff they
  share a root but have different hashes (a *fork*).
- **Qualified root**: `(root, account-or-zero)` — the globally unique identifier of a chain
  position and therefore of an election. There is at most one election per qualified root.
- **Representative weight** of an account: the total balance delegated to it, read from the
  ledger **at the moment a tally is computed** (live, never snapshotted at vote time). A
  delegation change affects in-flight elections.
- **Voting representative ("rep")**: an account on whose behalf this node signs votes
  (node holds its key and `enable_voting` is set).
- **Principal representative**: a rep whose weight exceeds the *principal threshold*
  (§3.3); only principal reps' votes are counted in elections and only they receive
  directed confirmation requests.

### 1.1 Online weight and quorum

Let `W_min` = configured `online_weight_minimum` (a hard floor on assumed online weight).

- **Online weight** `W_online`: the sum of current ledger weights of every rep observed to
  have voted within the last `2 × weight_interval`. A rep is only tracked if its weight
  exceeds `representative_vote_weight_minimum` (default 10 nano).
- **Trended weight** `W_trend`: the **median** of periodic online-weight samples retained
  over a trailing window (`weight_cutoff`, ≈2 weeks live / 1 day beta). Samples are taken
  every `weight_interval`; sampling is **skipped** while online weight is below `W_min`.
  `W_trend` is floored at `W_min`.
- **Effective weight**: `W_eff = max(W_online, W_trend, W_min)`.
- **Quorum delta**: `Δ = W_eff × QUORUM% / 100`, with `QUORUM% = 67` (fixed, not
  configurable). `Δ` is guaranteed never to fall below `W_min × 67 / 100`.

`Δ` is the single quorum quantity used by every confirmation decision (§5).

---

## 2. Vote object and validity

A vote is `{ account, signature, timestamp, hashes[] }`, `hashes` length 1..255.

### 2.1 Timestamp encoding

The 64-bit `timestamp` field encodes both a time and a duration hint:

- The low 4 bits are a **duration code** `d`; the timestamp granularity is therefore 16 ms.
  The advertised aggregation duration is `2^(d+4)` ms (range 16 ms … ~524 s).
- The remaining bits are milliseconds since the Unix epoch (low 4 bits masked to 0).
- The reserved value `0xFFFF_FFFF_FFFF_FFFF` (all ones) is the **final-vote** marker. A
  final vote is not a time; it is a commitment (§4). Its effective duration code is `0xF`.

`effective_timestamp(v)` = the all-ones value if final, else the time bits with duration
masked off.

### 2.2 Signature

The signed digest is `BLAKE2b("vote " ‖ hashes ‖ timestamp)`. A vote is **valid** iff the
signature verifies against `account`. Invalid votes are discarded before they reach any
election.

### 2.3 Vote ordering (per representative, per election)

Within one election a rep has at most one *current* vote. A newly received vote from rep
`R` for hash `H` at timestamp `T` is compared to `R`'s stored vote `(T0, H0)`:

1. `T < T0` → **replay** (reject, do not count).
2. `T == T0` and **not** `H0 < H` → **replay**. (Equal timestamp ties are broken in favor
   of the lexicographically **greater** hash; a final timestamp `0xFF…F` is strictly
   greater than every normal timestamp.)
3. Otherwise the vote *supersedes* and replaces `R`'s entry.

This makes a rep's accepted votes strictly monotonic in `(timestamp, hash)`; a final vote
always supersedes any earlier normal vote from the same rep.

### 2.4 Eligibility to be counted

- The voting rep's weight must exceed the **principal threshold** (§3.3); otherwise the
  vote is *indeterminate* and contributes no weight (this gate is disabled on dev).
- **Cooldown** (anti-spam, applied only to live — non-cache-sourced — non-final votes):
  the rep's previous accepted vote in this election must be older than `cooldown(weight)`:
  - weight > 5% of trended online weight → 1 s
  - weight > 1% → 5 s
  - otherwise → 15 s

  A vote that *raises* the rep to a final timestamp bypasses cooldown. Votes replayed from
  the local vote cache bypass cooldown.

An accepted vote updates the rep's `(time, timestamp, hash)` and triggers a confirmation
re-evaluation (§5).

---

## 3. Vote intake and routing

### 3.1 Inbound pipeline

Received vote bundles are signature-verified, then queued and drained fair-queued by
representative tier so that higher-weight reps' votes are processed first under load.
Verified votes are then *routed*.

### 3.2 Routing by hash

For each hash `H` in a vote:

- If an active election is connected to `H` → the vote is delivered to that election
  (§2.3–2.4, §5).
- Else if `H` is in the **recently-confirmed** cache → result `late` (already decided).
- Else → result `indeterminate`, and the vote is inserted into the **vote cache**
  (unless it originated from the cache itself).

A hash present in the vote but filtered out, or duplicated within the bundle, is ignored.

### 3.3 Principal threshold

`principal_threshold = W_trend / 1000` (i.e. 0.1% of trended online weight;
`principal_weight_factor = 1000`). Used both to gate counted votes (§2.4) and to select
recipients of confirmation requests (§6).

### 3.4 Vote cache (pre-election accumulation)

Votes for hashes with no active election are accumulated per hash:

- Up to `max_voters = 64` highest-weight reps retained per cached hash; a new heavier
  voter evicts the lightest.
- Two running sums per hash: total `tally` and `final_tally` (sum over reps whose cached
  vote is final).
- Cache holds up to `max_size = 65 536` hashes; entries older than `age_cutoff = 15 min`
  are evicted.

The vote cache is the input to the hinted scheduler (§7.2) and is replayed into an
election when one is later created for the hash.

---

## 4. Outbound voting policy (when this node signs a vote)

A node that holds rep keys participates by producing **normal** and **final** votes for
the current winner of an election it is tracking. The rules below are mandatory; they are
the safety-critical half of consensus.

### 4.1 Dependency gate

A node may vote for a block **only if all of the block's ledger dependencies are
cemented** (the previous block on the chain, and for receive/send-source the linked
block). If dependencies are not cemented, no vote of any kind is produced.

### 4.2 Normal vote

If the dependency gate passes and **no** final vote has been persisted for this root:
produce a normal vote for the block's own hash, with the current epoch time and a default
duration code (`0x9`, ≈8192 ms aggregation hint).

If a final vote **was** previously persisted for this root, the normal-vote request is
**upgraded**: the node instead emits a *final* vote for the **already-recorded hash**
(never for a different hash). See §4.4.

### 4.3 Final vote

A final vote uses the all-ones timestamp and duration code `0xF`. A node may emit a final
vote for `(root, H)` only if it can **claim the root's persistent final-vote slot**:

- The persistent `final_votes` record for `root` is empty → claim it for `H`, persist,
  emit.
- The record already equals `H` → re-emitting the same final vote is allowed.
- The record exists and is a **different** hash `H' ≠ H` → **refused**. The node will
  never produce a final vote for a second, conflicting block at the same root.

This per-root single-final-vote record is durable (survives restarts) and is the node's
equivocation barrier: an honest node contributes final-vote weight to **at most one**
block per account chain position, forever.

### 4.4 When votes are emitted

- A newly created election may immediately broadcast a vote.
- While an election is in the `active` state it (re)broadcasts a vote for the winner at
  most once per `vote_broadcast_interval` (15 s live; dev 500 ms). If the election is
  confirmed or already has quorum (§5.3), the broadcast is a **final** vote; otherwise a
  **normal** vote.
- The first time an election reaches quorum, a final vote for the winner is generated
  immediately (independent of the periodic interval).
- **Reply path**: when a peer asks for this node's position on a root (a confirmation
  request), the node answers using the *read-only final reply rule*: if a final vote is
  persisted for the root, reply with a final vote for the **recorded** hash (which may
  differ from the queried block on a fork); else if the queried block is itself cemented,
  reply with a final vote for it; else fall back to the normal-vote rule (§4.2).

### 4.5 Local vote history

Votes this node has generated for a root are cached so that repeated confirmation requests
for the same root are answered from cache without re-signing. The cache holds, per root,
votes for a single hash only, unique per rep; a higher-timestamp vote replaces a lower one.
When an election's winning fork changes (§5.2), the cached votes for the superseded hash
are dropped.

---

## 5. The confirmation decision

An election tracks a set of candidate blocks (forks) sharing one root, a per-rep current
vote map, and a current `winner` (initially the block the election was opened with).

Every time a vote is accepted (§2) and the election is not yet confirmed, the following
procedure runs.

### 5.1 Tally

For every counted rep vote, add the rep's **current ledger weight** to the tally of the
hash it voted for. Separately accumulate, for the **current winner hash**, the
`final_weight` = sum of weights of reps whose current vote for that hash is **final**.

`tally` is the per-block weight map; `T1` = greatest block tally, `T2` = second-greatest
(0 if only one block has any weight).

### 5.2 Winner selection / fork switch

The provisional winner is the block with the greatest tally. If

```
Σ(all block tallies)  ≥  Δ      AND      argmax-tally block ≠ current winner
```

then the election **switches** its winner to the higher-tallied block: the superseded
winner's locally generated votes are dropped (§4.5) and the new winning block is
re-asserted into the ledger (forced reprocess) so it can be cemented if it confirms.

Rationale: the winner only flips once enough total weight (≥ `Δ`) is present to make the
comparison meaningful, preventing flapping on sparse early votes.

### 5.3 Quorum test

The election **has quorum** iff:

```
T1 − T2  ≥  Δ
```

i.e. the leading block's weight **lead over the next block** is at least the quorum delta.
(A bare majority is insufficient; the *margin* must reach `Δ`. With no second block,
`T2 = 0` and the test reduces to `T1 ≥ Δ`.)

On the **first** transition into "has quorum", if this node is a voting rep it broadcasts a
**final** vote for the winner, and latches a `quorum-reached` flag (used by §6).

### 5.4 Confirmation (irreversibility)

The election is **confirmed** when **both** hold:

1. `T1 − T2 ≥ Δ` (quorum margin, §5.3), **and**
2. `final_weight ≥ Δ` — the winner has accumulated **final-vote** weight of at least the
   quorum delta.

Both conditions are necessary. Confirmation therefore requires that ≥ 67% of effective
online weight has cast an **irrevocable (final)** vote for one specific block at this
root, *and* that block leads any competitor by ≥ 67%.

On confirmation the winner is re-asserted into the ledger, the election is marked
confirmed, its `(root → winner)` pair enters the **recently-confirmed** cache, and the
winner is handed to the cementing subsystem (§8). Confirmation is idempotent and one-way.

---

## 6. Soliciting votes (liveness mechanism)

While an election is `active`, on each scheduler tick it may:

- **Re-broadcast the winning block** to principal reps that have not yet voted for it (or
  voted for a different fork), at most once per `block_broadcast_interval` (150 s live; dev
  500 ms) or immediately when the winner changes. Block broadcasts are capped per tick.
- **Send confirmation requests** for `(winner-hash, root)` to principal reps, throttled to
  one request per `confirm_req_time`: `5 × base_latency` for manual/priority/hinted,
  `2 × base_latency` for optimistic (`base_latency` = 1000 ms live, 25 ms dev). A rep is
  asked only if it has not voted, or — before quorum is latched — has not voted **final**,
  or has voted for a different hash. Requests are batched per channel (≤255 roots/message).

These steps never change the confirmation rule; they only accelerate vote collection.

---

## 7. Election creation (inputs to consensus)

An election for a root is created on demand by one of four schedulers. The scheduler only
decides *whether and when* an election starts and with what *behavior class*; it does not
affect the confirmation rule. A root that is in the recently-confirmed or recently-cemented
cache is **not** re-opened.

Behavior classes differ only in lifetime and container budget:

| Behavior   | Time-to-live | Container budget (of AEC size `S`, default 5000) |
|------------|--------------|--------------------------------------------------|
| priority   | 300 s        | up to `S`                                        |
| manual     | 300 s        | unbounded                                        |
| hinted     | 30 s         | 20% of `S`                                       |
| optimistic | 30 s         | 10% of `S`                                       |

Total in-flight winners pending cementing is independently capped (`max_election_winners`,
default 16384).

### 7.1 Priority

Unconfirmed blocks whose dependencies are cemented are queued, partitioned into balance
buckets (exponential balance ranges), ordered within a bucket by block timestamp, and
admitted as `priority` elections subject to per-bucket and global vacancy. This is the
normal path for ordinary traffic.

### 7.2 Hinted

The vote cache (§3.4) is scanned (every 1 s, or sooner when ≥20% of the hinted budget is
free). A cached hash qualifies when its cached `tally ≥ hinting_threshold`, where
`hinting_threshold = 10% × W_trend` (`hinting_threshold_percent = 10`, configurable). If
the cached `final_tally ≥ Δ` the block is activated directly (it can confirm on the
replayed cached votes alone); otherwise its uncemented dependency chain is activated first.
A block that fails to start is cooled down for 10 s.

### 7.3 Optimistic

Accounts whose unconfirmed depth `block_count − confirmation_height` exceeds
`gap_threshold = 16`, or which have an unconfirmed open block, become candidates; the first
unconfirmed block is activated as an `optimistic` election to confirm a chain ahead of
explicit demand. Bounded candidate queue (4096).

### 7.4 Manual

An explicit (RPC/internal) request to confirm a specific block. Unbounded, transitions to
`active` immediately.

---

## 8. Election lifecycle and cleanup

States and the **only** permitted transitions:

```
passive ──▶ active ──▶ confirmed ──▶ expired_confirmed   (terminal, cleaned up)
   │           │
   ├───────────┴────────▶ expired_unconfirmed             (terminal, cleaned up)
   │
   └──(any state)───────▶ cancelled                       (terminal, cleaned up)
```

- **passive**: listen-only; no broadcasts/requests. Lasts `5 × base_latency` (5 s live;
  ~125 ms dev). Skipped entirely (→ `active` immediately on creation) if there are **no**
  cached votes for the block, to avoid latency for fresh traffic.
- **active**: performs §6 and outbound voting (§4.4).
- **confirmed**: §5.4 reached; keeps listening for one tick, ensures the winner is
  broadcast, then → `expired_confirmed` and is removed.
- **expired_unconfirmed**: reached when the behavior's time-to-live (§7) elapses without
  confirmation. The root may be re-opened later by a scheduler.
- **cancelled**: forced teardown — e.g. the block (or a conflicting block) was cemented by
  another path, the block was rolled back, or a periodic check found the position already
  cemented. Cancelled immediately.

Other lifecycle rules:

- At most **10 candidate blocks** per election. An 11th block is admitted only by
  *replace-by-weight*: using vote-cache tallies, the lowest-tallied non-winner is evicted
  if the incoming/competing block has greater cached weight; the **current winner is never
  evicted**. If no eviction is possible the new block is rejected.
- A conflicting block discovered for an existing election's root is attached to that same
  election (subject to the 10-block rule) and its votes begin to count.
- An election may be **upgraded** to `priority` behavior (e.g. from hinted/optimistic) to
  get immediate vote broadcasting; this resets vote-broadcast throttling.
- A periodic checkup cancels elections whose chain position is already cemented (after a
  minimum run time) and flags long-running ("stale") elections for auxiliary recovery
  (e.g. bootstrap) without changing the confirmation rule.

---

## 9. From confirmation to irreversibility (cementing)

Confirmation (§5.4) marks the *decision*; cementing makes it durable in the ledger:

- The confirmed winner is enqueued for cementing. Cementing advances the account's
  confirmation height to include the block and any now-determined dependents.
- Cementing a block **implicitly resolves** any election on the same root and **cancels**
  dependent elections whose blocks become cemented. This is how chains confirm in bulk and
  how the system avoids redundant elections.
- The outcome is recorded in a recently-cemented cache with a classification:
  - *active confirmed quorum* — cemented because its own election reached §5.4;
  - *active confirmation height* — cemented as a dependency while an election existed;
  - *inactive confirmation height* — cemented as a dependency with no election.
- recently-confirmed / recently-cemented caches (each ~65 536 entries) suppress re-opening
  a decided root.

---

## 10. Safety and liveness properties

These follow from the rules above and the standard Nano assumptions.

**Safety — no two conflicting blocks at one root are both confirmed.** §5.4 requires
`final_weight ≥ Δ = 67% × W_eff` for the confirmed block. §4.3 guarantees an honest node
ever contributes final-vote weight to **at most one** block per root, and that decision is
persisted across restarts. Two conflicting blocks could both reach `final_weight ≥ 67%`
only if more than `2×67% − 100% = 34%` of effective online weight **double-final-votes**
(equivocates) at the same root — which the persistent single-final-vote record prevents
honest nodes from doing. Hence confirming conflicting blocks requires an adversary
controlling > 34% of effective online weight to actively equivocate. The dependency gate
(§4.1) further ensures a block is only ever voted once its prerequisites are themselves
irreversible, so confirmation order respects chain order.

**Liveness — progress under honest majority.** A block confirms once its lead reaches `Δ`
and final-vote weight reaches `Δ`. With ≥ 67% of effective online weight honest, online,
and converging on one block, both thresholds are met. The quorum floor (`Δ ≥ W_min×67/100`)
prevents an attacker from driving the threshold to zero by suppressing observed online
weight; conversely, sampling pauses below `W_min` rather than trending toward an unsafe
low quorum. An adversary holding > 33% of effective online weight can **stall** liveness
(prevent the 67% margin) but cannot violate safety.

**Weight is live.** All tallies use ledger weight at evaluation time, so re-delegation
takes effect on in-flight elections; quorum tracks `W_eff` continuously.

---

## 11. Constant reference (live network)

| Quantity | Value | Configurable |
|---|---|---|
| Quorum percentage | 67% | no |
| `principal_weight_factor` (principal threshold = trended/1000) | 1000 (0.1%) | no |
| `representative_vote_weight_minimum` | 10 nano | yes |
| `online_weight_minimum` (`W_min`) | configured floor | yes |
| Weight sample interval / retention | `weight_interval` / ≈2 weeks | yes |
| Quorum margin rule | `T1 − T2 ≥ Δ` | no |
| Confirmation extra rule | `final_weight ≥ Δ` | no |
| Max forks per election | 10 | no |
| Vote hashes per message | ≤ 255 | no |
| Vote cooldown (by weight) | 1 s / 5 s / 15 s (>5% / >1% / else) | no |
| `base_latency` | 1000 ms (dev 25 ms) | no |
| passive duration | `5 × base_latency` (5 s) | no |
| `vote_broadcast_interval` | 15 s (dev 500 ms) | yes |
| `block_broadcast_interval` | 150 s (dev 500 ms) | yes |
| confirm-req interval | `5×base` (manual/priority/hinted), `2×base` (optimistic) | no |
| TTL | 300 s priority/manual; 30 s hinted/optimistic | no |
| AEC size `S` | 5000 | yes |
| hinted / optimistic budget | 20% / 10% of `S` | yes |
| `max_election_winners` | 16384 | yes |
| recently-confirmed / cemented cache | 65 536 each | yes |
| hinting threshold | 10% of trended weight | yes |
| optimistic gap threshold | 16 blocks | yes |
| vote cache: max hashes / voters-per-hash / age | 65 536 / 64 / 15 min | yes |

---

## 12. Notes on this repository's current state

- `vote_spacing` exists in the tree but is **not wired into the live voting path**; the
  current equivocation barrier is the persistent per-root `final_votes` record (§4.3),
  not time-based spacing. A re-implementation should follow §4.3, not vote spacing.
- The persistent final-vote claim (§4.3) and the dependency-cemented gate (§4.1) are the
  two rules most easily missed in a re-implementation and are individually sufficient to
  break safety if omitted.
