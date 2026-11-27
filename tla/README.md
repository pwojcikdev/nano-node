# Nano confirmation model

A TLA+ model of how Nano nodes confirm blocks through representative votes, together with scenarios that the TLC model checker explores exhaustively. It follows the node at the head of the v29 development line (commit `c3793c96c3`).

The scenarios ask two questions of small networks under any ordering and delay of messages:

- Can two nodes cement different blocks at the same root?
- Can a released block stay undecided forever, and if so, what gets it out?

In short: under the node's rules no scenario reaches two nodes with different cemented blocks at a root while byzantine weight stays below 34 of 100, less than 34 changes its representative while a root is contested and the nodes keep their trended weight. A root that an account forked can stay undecided forever with every representative honest and online. Where enough weight votes, roots without a fork are always decided, and so are chains that do not build on an unconfirmed block of another account. [Findings](#findings) has the details.

## Layout

| File | Content |
|---|---|
| `NanoConsensus.tla` | The model: state, steps, the properties checked |
| `MC_*.tla`, `MC_*.cfg` | One scenario each: accounts, blocks, weights, representatives, bounds |
| `checks.txt` | The checks: scenario, property, expected outcome |
| `checks-slow.txt` | The checks that take longer, for a dedicated runner |
| `check.sh` | Runs the checks with TLC and compares outcomes with expectations |

## Running

`check.sh` needs Java 11 or later and fetches `tla2tools.jar` on first use.

```bash
./check.sh                           # every check in checks.txt
./check.sh fork                      # the checks whose id contains "fork"
./check.sh -t fork-split             # also print the trace TLC found
CHECKS=checks-slow.txt ./check.sh    # the slow ones
```

`JAVA`, `TLA2TOOLS`, `WORKERS`, `HEAP` and `LIMIT` in the environment select the Java binary, the tools jar, the worker threads, the heap size and a time limit per check. Logs and the generated configurations go to `.tlc/`. On 14 threads no check in `checks.txt` takes more than about a quarter of an hour and the whole list takes about an hour and a quarter. `checks-slow.txt` holds the checks that take longer and is meant for a dedicated runner.

A scenario's own configuration also runs as it is in the TLA+ Toolbox or the VS Code extension; it checks the properties that must hold. `checks.txt` derives the other checks from it by replacing constants and adding one property.

## What is modelled

Every node keeps a ledger and a set of cemented blocks. Blocks with the same account and the same previous block share a root and are forks of each other; a ledger holds at most one block per root, and everything that block depends on. The network is a set of released blocks and a set of votes that only grow. A node may receive any of them at any time, in any order, any number of times, which covers delay, loss with retransmission, the vote cache, rebroadcasting and solicited replies.

The rules that decide confirmation, as the node implements them:

1. Representative weights follow every block in a node's ledger, cemented or not, so two nodes can weigh the same representative differently.
2. An election holds the ledger block of its root and the forks the node knows. For every representative it keeps the latest vote, ordered by timestamp and then by block hash. A final vote ranks above every other vote. A vote for a block the election does not hold does not reach it.
3. A tally weighs the recorded votes with the node's own weights. The winner moves to the heaviest voted block once the recorded votes add up to the node's quorum delta; equal weights go to the higher hash.
4. An election has quorum when its winner leads every other voted block by the full delta, and final quorum when the final votes for the winner add up to delta.
5. A voter votes for the winner of its election when that block is in its ledger and everything the block depends on is cemented. The vote is non-final until the election has quorum. At quorum the voter asks for a final vote, which is granted unless a final vote for another block of the root is on record. The record is never removed, and with it on record the voter answers every later request to vote at that root with the recorded final vote.
6. A voter asked about a block answers with a final vote only: the one on record for the root, or one for the block it has cemented there.
7. An election confirms its winner when it has quorum and final quorum at once. The node then cements the winner and everything it depends on.
8. The quorum delta is 67% of the largest of the online weight the node observes, its trended online weight and the configured minimum.
9. A representative counts towards the online weight from the moment one of its votes reaches an election of the node, or it answers a probe, until the node has not heard from it for ten minutes. An election ends after five minutes at most, so no election holds the vote of a representative its node has stopped counting.

| Model | Node |
|---|---|
| `ReceiveBlock` | `block_processor::process_one`, `ledger_processor`, `active_elections::handle_fork`, `fork_cache` |
| `Started`, `StartElection`, `Activatable` | `active_elections::insert`, `scheduler::priority::activate`, `scheduler::hinted`, `scheduler::optimistic` |
| `ReceiveVote`, `Supersedes` | `vote_router::vote`, `election::vote`, `election_ballot::vote` |
| `TallyOf` | `election_ballot::evaluate` |
| `React` | `election::confirm_if_quorum`, `election::evaluate_locked`, `block_processor::force`, `block_processor::rollback_competitor`, `ledger::rollback`, `cementing_set::run_batch`, `ledger::cement`, `active_elections::block_cemented` |
| `Reevaluate` | `election::tick`, `election::broadcast_vote_locked` |
| `VoteNormal` | `vote_generator::process_normal`, `voting_policy::vote` |
| `ProcessFinal` | `vote_generator::process_final`, `voting_policy::vote_final`, the final vote store |
| `ReplyFinal` | `vote_replier::process`, `voting_policy::reply_final` |
| `Weight` | `ledger::weight` |
| `DeltaOf`, `Observe`, `Trim`, `Sample` | `online_reps::delta`, `online_reps::observe`, `online_reps::trim` and its wait against `election::time_to_live`, `online_reps::sample`, `rep_crawler` |
| `Expire` | The time to live in `election::tick`, `scheduler::bucket::cancel_lowest_election` |
| `EvictFork` | `fork_cache::put` |
| `Rollback` | `bounded_backlog::perform_rollbacks` |
| `Crash`, `Recover` | A node restart: the ledger, the cemented blocks, the final votes and the trended weight are stored, everything else is lost |

Account owners are part of a scenario. An honest owner never signs two blocks at one root and receives a send only once it is cemented. An optional one does the same but may never release its blocks. A careless one receives a send as soon as it is released. A byzantine one signs anything at any time. Byzantine representatives have every vote on offer, final and non-final, and choose which node gets which.

## What is left out

- Capacity: the limits on active elections and their priorities, queue overflows and bandwidth limits. Every eligible block can have an election at every node. `Expire` stands in for an election that is dropped.
- Time: vote cooldown inside a ballot, the pacing of votes, broadcasts and requests, and the passive phase of an election. The model orders steps freely instead. The one order between timers it keeps is rule 9: `Trim` leaves a representative alone while a running election of the node holds a vote of it. The constant `StaleVotes` lifts that.
- The limit on blocks per ballot and the eviction it causes, the size and age limits of the vote cache.
- Principal representative thresholds and representative tiers.
- Wall clock vote timestamps. A voter's non-final timestamps count up from one, and a voter casts a new non-final vote only when its choice changes. `MaxTs` bounds them.
- The gap between an election asking the block processor or the cementing set for something and its completion. Both run to completion in the step that triggers them. The gap between asking for a final vote and issuing it is modelled.
- The gap between a voter issuing a vote and its own election counting it.
- The two week median behind the trended weight. `Sample` moves it to the current online weight in one step, so the model reaches in a step what takes the node more than a week.
- The conditions under which the bounded backlog leaves a block alone, other than the block having an election: cached votes for it and a place in the scheduler.
- The weight table shipped with a release. `ledger::weight` answers from it until the node has synced past a block count, so such a node ignores the weight that the blocks in its ledger move.
- The vote a voter casts right after its election switched to another block. The node casts the vote it asked for at the switch without tallying again; the model casts a non-final vote only while a fresh tally still names that block. The two differ only where the switch itself moves weight, as it does when the forked account holds voting weight.
- Bootstrapping, pruning, epochs, signatures and work. Bootstrapping is covered by blocks being receivable at any time.

## Properties

These must hold in every reachable state:

- `Agreement`: no two nodes cement different blocks at one root.
- `FinalVotesUnique`: a voter never issues final votes for two blocks at one root.
- `WellFormed`: ledgers hold one block per root with all dependencies, cemented blocks have cemented dependencies, elections only hold what they were given.

These probe for conditions of interest; a violation comes with the shortest trace that leads there:

- `NoFinalVoteSplit`: voters never issue final votes for different blocks at one root.
- `EveryRootWinnable`: at every root some block can still gather a delta of final votes, counting every voter that has no final vote for another block there.
- `FinalQuorumConfirms`: an election whose winner has a full delta of final votes can confirm it.
- `NoDeadEnd`: the nodes never run out of steps while a released block is undecided. A block is decided at a node once it is cemented there, or once a competitor of it or of a block it builds on is.
- `HonestNeverStuck`: the same for the blocks of honest owners alone.
- `EventuallySettled`: every fair run decides every released block.

The steps are split into the ones nodes and well behaved owners take on their own (`Protocol`) and the ones left to the adversary, to operators and to the passing of time (`Environment`): node restarts, elections timing out, cache evictions, backlog rollbacks, late or byzantine releases, the delivery of byzantine votes, a running or byzantine representative dropping out of a node's online set. A dead end is a state where no `Protocol` step is possible. Fairness applies to `Protocol` only.

## Scenarios and results

Weights are out of 100 and delta is 67 unless a scenario moves it. "States" counts the distinct states TLC explored; a check that finds a violation stops at the first one. A violated probe is a result, not a failure of the check: `checks.txt` records the outcome each check is expected to have, and every check below has it.

### No forks: `MC_Baseline`

A send, its receive and a change of representative move weight between three voters (40, 30, 30) while the three blocks are confirmed. Every owner is honest.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `baseline-safety` | The properties that must hold | hold | 98,875 |
| `baseline-no-dead-end` | `NoDeadEnd` | holds | 98,875 |
| `baseline-live` | `EventuallySettled` | holds | 98,875 |
| `baseline-restart` | `NoDeadEnd`, with one node restart | holds | 1,583,508 |
| `baseline-restart-live` | `EventuallySettled`, with one node restart | holds | 1,583,508 |
| `baseline-dropped` | `NoDeadEnd`, with one election timeout and one backlog rollback | holds | 2,557,377 |

### A fork between two voters: `MC_Observer`

One account signs two blocks at a root. Two voters of 60 and 40 hold the weight, a third node only follows.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `observer-safety` | `NoFinalVoteSplit` | holds | 6,609 |
| `observer-no-dead-end` | `NoDeadEnd` | holds | 6,609 |
| `observer-live` | `EventuallySettled` | holds | 6,609 |
| `observer-restart` | `NoDeadEnd`, with one restart, election timeout and cache eviction | holds | 1,642,759 |
| `observer-restart-live` | `EventuallySettled`, with the same | holds | 1,642,759 |
| `observer-rollback` | `NoDeadEnd`, with one election timeout and one backlog rollback | violated | 235,646 |
| `observer-all-faults` | The properties that must hold, with one restart, election timeout, cache eviction and backlog rollback | hold | 10,334,731 |

### Blocks built on a fork: `MC_Successors`

The same two voters. The account signs one more block on top of each of its two blocks, and elections also start on blocks whose dependencies are not cemented.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `successors-safety` | The properties that must hold | hold | 1,665 |
| `successors-no-dead-end` | `NoDeadEnd` | holds | 1,665 |
| `successors-live` | `EventuallySettled` | holds | 1,665 |
| `successors-rollback` | The properties that must hold, with one election timeout and one backlog rollback | hold | 52,114 |

### A fork between three voters: `MC_Fork`

One account signs two blocks at a root. Three honest voters (34, 33, 33) stay online.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `fork-safety` | The properties that must hold | hold | 118,646 |
| `fork-safety-deeper` | The same with four non-final votes per voter instead of two | hold | 288,490 |
| `fork-safety-rollback` \* | The same with one election timeout and one backlog rollback | hold | 7,176,993 |
| `fork-split` | `NoFinalVoteSplit` | violated | 22,204 |
| `fork-hopeless` | `EveryRootWinnable` | violated | 59,911 |
| `fork-dead-end` | `NoDeadEnd` | violated | 93,820 |
| `fork-live` | `EventuallySettled` | violated | 221,719 |
| `fork-blocked-final` | `FinalQuorumConfirms` | violated | 6,390 |

### A fork between four voters: `MC_FourVoters`

The same fork between four honest voters of 25 each, so that quorum takes three of them.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `four-voters-split` | `NoFinalVoteSplit` | violated | 96,063 |
| `four-voters-hopeless` | `EveryRootWinnable` | violated | 2,496,867 |

### After a split: `MC_Bound`

Starts where a representative of weight k has its final vote on record for block B, a second one holding 90 - k has its final vote for block A and a third one holding 10 has not voted. Every ledger holds A and every node knows both blocks.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `bound-16-settles` | `NoDeadEnd`, k = 16 | holds | 532 |
| `bound-30-dead-end` | `NoDeadEnd`, k = 30 | violated | 868 |
| `bound-30-ordering` | No node ever cements at the root, k = 30 | violated | 210 |
| `bound-35-stuck` | No node ever cements at the root, k = 35, with restarts, election timeouts and cache evictions | holds | 111,975 |
| `bound-35-moved` | No node ever cements at the root, k = 35, once 20 of it may move to another representative | violated | 157 |

### A split, then offline weight returns: `MC_Returning`

Starts like `MC_Bound`: a representative of weight k has its final vote on record for block B and a second one holding 85 - k has its final vote for block A. A third one holding 15 was offline and comes back. The two running nodes had stopped counting it as online, so their delta is 56, which is 67% of the 85 they see.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `returning-16-settles` | `NoDeadEnd`, k = 16 | holds | 89,296 |
| `returning-16-live` | `EventuallySettled`, k = 16 | holds | 89,296 |
| `returning-30-dead-end` | `NoDeadEnd`, k = 30 | violated | 72,474 |
| `returning-30-ordering` | No node ever cements at the root, k = 30 | violated | 1,580 |
| `returning-35-stuck` | No node ever cements at the root, k = 35 | holds | 48,425 |
| `returning-35-fixed` | The same with online weight tracking off, so that delta stays at 56 | violated | 383 |

### A byzantine representative: `MC_Byzantine`

One account signs two blocks at a root. Two honest voters share the weight a byzantine representative does not hold.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `byzantine-33-safety` | The properties that must hold, byzantine weight 33 | hold | 17,030 |
| `byzantine-34-agreement` | The same, byzantine weight 34 | `Agreement` violated | 5,171 |
| `byzantine-20-dead-end` | `NoDeadEnd`, byzantine weight 20 | violated | 2,179 |
| `byzantine-20-no-fork` | `NoDeadEnd`, byzantine weight 20, a single block at the root | holds | 164 |
| `byzantine-2-split` | `NoFinalVoteSplit`, byzantine weight 2 | violated | 6,024 |

In the checks below the nodes track the online weight with a minimum of 60, and their trended weight at the start is the honest weight alone: the byzantine representative has been away for long enough to drop out of it. It shows up and falls silent as it likes.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `byzantine-33-absent` | The properties that must hold, byzantine weight 33 | hold | 98,744 |
| `byzantine-33-expiring` | The same with one election timeout | hold | 1,144,088 |
| `byzantine-14-absent` | The same, byzantine weight 14 | hold | 61,984 |
| `byzantine-14-stale` | The same, byzantine weight 14, without rule 9 | `Agreement` violated | 59,763 |
| `byzantine-13-stale` | The same, byzantine weight 13, without rule 9 | hold | 165,424 |

### A byzantine representative next to three voters: `MC_ByzantineThree`

The same fork with three honest voters. With the byzantine representative at 33% they hold 34%, 16.5% and 16.5%, so the heaviest one confirms with the byzantine weight alone and the two lighter ones cannot. Balances are in half percents, which puts delta at 134 of 200.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `byzantine-three-34` | The properties that must hold, byzantine weight 34% | `Agreement` violated | 1,331,920 |

### A partition: `MC_Partition`

One account signs two blocks at a root while two voters of 50 each may lose sight of each other. Nodes track the online weight and derive delta from it.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `partition-history` | The properties that must hold, minimum 60, trended weight 100 | hold | 1,492 |
| `partition-no-history` | The same, minimum 60, no trended weight | `Agreement` violated | 8,580 |
| `partition-high-minimum` | The same, minimum 77, no trended weight | hold | 5,968 |
| `partition-low-minimum` | The same, minimum 50, trended weight 100 | `Agreement` violated | 11,327 |

### Weight that never votes: `MC_Offline`

No forks. 40 of the weight is delegated to a representative that never votes, the trended weight still counts it.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `offline-recovers` | `NoDeadEnd`, minimum 50 | holds | 89 |
| `offline-live` | `EventuallySettled`, minimum 50 | holds | 89 |
| `offline-halts` | `NoDeadEnd`, minimum 70 | violated | 15 |

### Weight moving during a fork: `MC_Redelegation`

One account signs two blocks at a root while a balance moves from one of two voters to the other through a change block.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `redelegation-33-safety` | The properties that must hold, 33 moving | hold | 41,800 |
| `redelegation-34` | The same, 34 moving | `Agreement` violated | 2,440 |
| `redelegation-33-split` | `NoFinalVoteSplit`, 33 moving | violated | 14,660 |
| `redelegation-33-stuck` | `NoDeadEnd`, 33 moving | violated | 31,697 |

### Around a send that cannot be confirmed: `MC_Dependencies`

Starts where the two voters have their final votes on record for different sends of one account. The destination of one send receives it before it is cemented; an unrelated honest account changes its representative.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `dependencies-safety` | The properties that must hold | hold | 2,000 |
| `dependencies-honest` | `HonestNeverStuck` | holds | 2,000 |
| `dependencies-careless` | The receive is decided at every node | violated | 2,000 |

### A forked send and its receive: `MC_ForkedSend`

Starts from nothing. One account signs two sends at a root, to different accounts, and the destination of one receives it as soon as it is released. Three honest voters (34, 33, 33) stay online. The balance sent carries no voting weight, and elections also start on blocks whose dependencies are not cemented.

| Check | Asks | Outcome | States |
|---|---|---|---|
| `forked-send-safety` | The properties that must hold | hold | 1,329,382 |
| `forked-send-receive` | The receive is decided at every node | violated | 534,500 |

\* in `checks-slow.txt`

### Slow checks: `checks-slow.txt`

These take longer than the checks in `checks.txt`. `fork-safety-rollback` above is one of them and has run to completion. The others have not, so their outcomes in the list are expectations; a check joins the tables above once a run of it finishes. Each was stopped where the last column says, without a violation by then.

| Check | Asks | Stopped at |
|---|---|---|
| `fork-rollback-deeper` | The properties that must hold for `MC_Fork` with one election timeout, one backlog rollback and four non-final votes per voter instead of two | 5.8 million states, 10 minutes |
| `fork-safety-restart` | The properties that must hold for `MC_Fork` with one restart, election timeout and cache eviction | 8.4 million states, 10 minutes |
| `four-voters-safety` | The properties that must hold for `MC_FourVoters` | 19.5 million states, 50 minutes |
| `byzantine-three-safety` | The properties that must hold for `MC_ByzantineThree` with the byzantine representative at 33% | 4.7 million states, 10 minutes |
| `second-fork-safety` | The properties that must hold for `MC_SecondFork`: the account of `MC_Fork` signs two more blocks on top of the first of its two. Three honest voters (34, 33, 33) stay online, and the second pair has its elections at a node once that node has cemented the block under it. | 5.7 million states, 10 minutes |

## Findings

### Agreement holds

No scenario with honest voters, less than 34 byzantine weight, less than 34 of weight that two ledgers count for different representatives and an intact trended weight reaches two nodes that cemented different blocks at one root. That covers forks under every message ordering, blocks built on either side of a fork, restarts, election timeouts, backlog rollbacks, up to 33 of the weight changing its representative during an election and blocks depending on blocks of other accounts. The one check that gets there with less, `byzantine-14-stale`, switches rule 9 off on purpose.

At 34 the byzantine representative gets there by giving a final vote for one block to half of the honest weight and one for the other block to the rest. The halves can be one voter each (`byzantine-34-agreement`) or one against two (`byzantine-three-34`). In the second trace the heavier voter first votes for A, moves to B on the byzantine vote and cements it, while the two lighter ones still count its earlier vote for A, reach quorum with the byzantine vote for A and cement that.

### Honest voters can split their final votes

Rule 5 lets a voter issue its final vote when its own election shows the winner leading by delta. That election is one node's view of non-final votes that are still moving, so two voters can each see such a lead for a different block. `fork-split` finds it with three honest voters that stay online (34, 33, 33):

| | n1 (34) | n2 (33) | n3 (33) |
|---|---|---|---|
| Blocks arrive | holds B, knows A | holds A, knows B | holds A |
| First votes | B | A | A |
| n2 tallies n1: B, n2: A | | 67 took part and B is heavier: switches to B | |
| n1 tallies n1: B, n2: A, n3: A | A is heavier: switches to A, votes A | | |
| n1 tallies its own new vote | A at 100: final vote for A | | |
| n2 votes B, its ballot still holds the first vote of n1 and nothing of n3 | | B at 67, A at 0: final vote for B | |

Once every vote has reached every node, A has 67 (n1 final, n3 non-final) and B has 33 (n2 final). All three ledgers hold A and all three elections name A the winner, but A leads by 34 where quorum needs 67, so n3 never issues a final vote and the final weight behind A stays at 34. `fork-dead-end` shows that no step is left in that state, and `fork-live` that a fair run stays there forever. Nothing in it is byzantine except the account that signed two blocks.

The account cannot get out on its own. Another block at the same root is one more fork, and by rule 5 no voter votes on a block built on either fork while that fork is not cemented.

More voters do not prevent it. Four honest voters of 25 each split as well (`four-voters-split`) and can end with two final votes on each side of the fork, after which neither block can gather a delta (`four-voters-hopeless`).

### What a split leaves behind

Rule 7 asks for quorum as well as final quorum. `fork-blocked-final` reaches an election whose winner has 67 of final weight while another block has 33, and which therefore does not confirm. While the other votes are non-final this passes, because their voters follow the heavier block. Final votes stay where they are.

Let k be the weight whose final votes are on record for blocks other than the one the rest of the weight favours, and W the weight that votes. Nothing can be confirmed when k exceeds W - delta, and the lead of rule 4 is out of reach when k exceeds half of that. With delta at 67 of 100:

| k | What follows | Checks |
|---|---|---|
| above 33 | No block can gather a delta of votes any more. Three honest voters get there with one final vote for A and two for B. Restarts, timeouts and forgotten forks change nothing. | `fork-hopeless`, `bound-35-stuck` |
| 17 to 33 | The favoured block can gather a delta of final votes, but an election that has recorded the votes of the k never has quorum again. A node issues its final vote, or confirms, only if its election tallies the other votes first; each restart of the election is another try. | `bound-30-dead-end`, `bound-30-ordering` |
| up to 16 | The favoured block leads by delta with everyone else behind it and is confirmed. | `bound-16-settles` |

The same arithmetic lets a byzantine representative with more than 16 of the weight keep a forked root unconfirmed for as long as it likes, by voting for the block the honest voters did not choose (`byzantine-20-dead-end`). Without a second block at the root it has nothing to vote for and the honest 80 confirm (`byzantine-20-no-fork`). A byzantine representative of any weight can also tip evenly divided honest voters into a split (`byzantine-2-split`).

### A voter can turn around after others issued their final votes

A non-final vote binds nothing. `Rollback` stands for the bounded backlog rolling back an uncemented block that has no election at that moment; the node may then take a competing block and vote for it. `observer-rollback` reaches a dead end with two honest voters this way:

1. The heavier voter (60) holds B and votes for it.
2. The lighter voter (40) follows, sees 100 behind B and issues its final vote for B.
3. The election of the heavier voter times out and its backlog rolls B back.
4. The heavier voter receives A first and votes for it.

A now has the 60 of the heavier voter and B the 40 of the lighter one, cast as a final vote. Neither reaches quorum, and the heavier voter never returns to B: by rule 3 its election stays with the heavier block, which its own vote makes A, although B is the only block that could still be confirmed. Without the rollback the same two voters always settle, also across restarts and election timeouts (`observer-no-dead-end`, `observer-live`, `observer-restart`, `observer-restart-live`).

The node's backlog also leaves a block alone while votes for it are cached or the scheduler holds it, which the model does not track. The step stands for a node that stayed saturated for long enough that neither applies.

### What gets a root out

- Weight moving away from the representatives bound to the other block, to representatives that can still vote for the favoured one. A node counts a change of representative as soon as the block is in its ledger, and with 20 of the 35 moved a node confirms (`bound-35-moved`). The check shows that one node gets there, not that every node does.
- The order in which an election tallies votes, when k is 33 or less (`bound-30-ordering`).
- Weight that was offline coming back, when k is 16 or less. Above that it does not settle the root; the next section has the cases.
- Outside the model: the operator of a bound representative removing the final vote record of that root.

### Weight that comes back online

`MC_Returning` adds 15 of weight that was offline during the split and returns. The two running nodes had stopped counting it as online, so before it returns their delta is 56, 67% of the 85 they see. The returning weight counts as online as soon as a node hears its vote, and delta rises to 67 in the same step.

| k | Before the return | After the return | Checks |
|---|---|---|---|
| 16 | A has 69 of final votes but leads by 53, short of 56, unless an election tallies them before the vote for B. | A reaches 84 and leads by 68 against a delta of 67. Every node confirms. | `returning-16-settles`, `returning-16-live` |
| 30 | A has 55 of final votes, one short of 56. | A can reach 70 against a delta of 67, but only in an election that has not recorded the 30 for B. In the trace of `returning-30-dead-end` the two running nodes get there and the returning one does not: it holds 70 of final votes for A and never confirms. | `returning-30-dead-end`, `returning-30-ordering` |
| 35 | A has 50 of final votes, 6 short of 56. | The returning 15 would have covered that, but delta has risen to 67 with them and A can reach only 65. Nothing is ever cemented. With delta held at 56, a node does cement A. | `returning-35-stuck`, `returning-35-fixed` |

Weight coming back online settles a split only while the weight bound to the other block is small.

### Only chains built on the unconfirmed block are affected

In `MC_Dependencies` a forked send can no longer be confirmed. The unrelated honest account is always confirmed (`dependencies-honest`). The account that received the send before it was cemented is not: its receive is rolled back at the node that held it when that node's election moves to the other send, no ledger can take it again, and it is neither cemented nor ruled out (`dependencies-careless`). An honest owner never signs that receive, so its chain keeps working.

`MC_ForkedSend` gets to the same place from nothing, with three voters and the receive released while the two sends compete. No two nodes cement different blocks on the way (`forked-send-safety`). The final votes of two voters can split between the sends; every ledger then holds the send that was not received, nothing is cemented, and no ledger can take the receive (`forked-send-receive`).

### The quorum delta is only as good as the trended weight

`MC_Partition` separates two voters of 50 each.

| Minimum online weight | Trended weight at the start | What follows | Check |
|---|---|---|---|
| 60 | 100 | Neither side confirms alone. | `partition-history` |
| 60 | none | Each side confirms its own block: delta falls to 67% of the minimum, 40. | `partition-no-history` |
| 77 | none | Neither side confirms alone: 67% of the minimum is more than half of the weight. | `partition-high-minimum` |
| 50 | 100 | Each side confirms its own block once its trended weight has followed its online weight down to 50. | `partition-low-minimum` |

Delta never falls below 67% of the minimum. Agreement under a partition therefore needs twice that to exceed the weight that can vote, or every node to keep a trended weight from before the partition. The model lets the trended weight move in one step; in the node it is the median of two weeks of samples.

`MC_Offline` is the other side of the same rule: with 40 of the weight delegated to a representative that never votes, the remaining 60 confirm nothing until the trended weight has followed the online weight down (`offline-recovers`, `offline-live`), and never if 60 is under the minimum (`offline-halts`).

### Elections have to end before a silent representative stops counting

A tally weighs every vote an election has recorded with the ledger weight of its representative, whether or not the node still counts that representative as online. Delta comes from the weight the node does count. Rule 9 keeps the two in step: an election ends after five minutes at most (`election::time_to_live`) and a representative counts for ten minutes after its last vote reached an election (`online_reps::trim`, twice `weight_interval`), so the weight behind every recorded vote is part of delta.

`MC_Byzantine` shows what that is worth. The byzantine representative has been away for long enough that the trended weight is the honest weight alone. It returns, gives each of the two honest voters a final vote for a different block and falls silent again. With rule 9 its weight counts towards delta for as long as its votes count, and 33 of it break nothing (`byzantine-33-absent`), also when an election times out, the representative drops out and the election starts again (`byzantine-33-expiring`). Without rule 9, 14 are enough: each honest voter holds 43 and sees 57 behind its own block against a delta of 57, which is 67% of the 86 that remain online (`byzantine-14-stale`). With 13 the same adds up to 57 against 58 (`byzantine-13-stale`).

Only the order of the two timers keeps the byzantine bound at 34 here. If an election could outlive the time a silent representative stays counted, a representative with about 14.5% of the weight (34 / 2.34) could make two halves of the honest weight cement different blocks. It would have to stay away for more than a week first, since the trended weight is a median of two weeks, and the two halves would have to miss each other's votes in the meantime.

### Weight that moves during a fork

`MC_Redelegation` moves a balance from one voter to the other with a change block while another account has two blocks at a root. A node that has the change block counts the balance for the new representative, a node that has not counts it for the old one. The owner of the balance signs one valid block and nothing else.

With 34 of the weight moving, each voter reaches quorum alone in its own view and the two cement different blocks (`redelegation-34`); with 33 they cannot (`redelegation-33-safety`). What matters is that the weight moves between the two decisions, not that the change block is unconfirmed: a voter that has cemented the change block and one that has not differ in the same way.

Far below 34, moving weight costs liveness. Two honest voters with fixed weights never split their final votes (`observer-safety`). With 33 moving they do (`redelegation-33-split`): one ledger weighs them 34 and 66, the other 67 and 33, so each sees the other as the heavier one and moves to the other's block. The other's first vote for that block is still in its ballot, so it sees 100 behind the block and issues its final vote. The two final votes name different blocks and the two voters reach a dead end (`redelegation-33-stuck`).

## Reading the evidence

TLC explores every reachable state of each scenario, so "holds" is a statement about that scenario and its bounds: two to four voters, one contested root, `MaxTs` non-final votes per voter and root, and the fault budget given. It is not a proof for larger networks.

The model allows every ordering of messages. A trace shows that an ordering leads somewhere, not how likely the network is to produce it.

## Adding a scenario

A scenario module extends `NanoConsensus` and defines the accounts with their starting balances and representatives, how each owner behaves, and every block that can ever be signed, forks included. Its configuration binds those definitions and the bounds to the constants of the model. A scenario that starts from a state other than the empty one defines its own initial predicate, as `MC_Bound` does. Lines in `checks.txt` then name the properties to check and the outcome to expect.
