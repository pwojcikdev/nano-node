---------------------------- MODULE NanoConsensus ----------------------------
(***************************************************************************)
(* Vote based confirmation of blocks in the Nano node.                     *)
(*                                                                         *)
(* Every node keeps a ledger of blocks, of which a prefix per account is   *)
(* cemented.  Blocks at the same root (same account, same previous block)  *)
(* are forks of each other, a ledger holds at most one of them.  A node    *)
(* runs one election per root; an election records the latest vote of each *)
(* representative, weighs the votes with the representative weights of the *)
(* node's own ledger and compares them with the node's own quorum delta.   *)
(* A representative issues a final vote once its election sees quorum, a   *)
(* final vote is recorded durably and never changes.  An election confirms *)
(* its winner once it has quorum and enough final vote weight stands       *)
(* behind the winner, and the node then cements the winner together with   *)
(* everything it depends on.                                               *)
(*                                                                         *)
(* The network is a set of blocks and a set of votes that only grow.  A    *)
(* node may receive any of them at any time, in any order, any number of   *)
(* times, which covers delay, loss with retransmission, vote caching,      *)
(* rebroadcasting and solicited replies.                                   *)
(*                                                                         *)
(* Each action names the node code it mirrors.  README.md lists what is    *)
(* left out.                                                               *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    Nodes,          \* Honest nodes; a node's id doubles as its representative account
    Voters,         \* Nodes that vote
    Byzantine,      \* Representative accounts controlled by the adversary
    BlockDef,       \* Every block that can ever be signed, by id: [acct, prev, kind, link, rep, bal]
    HashRank,       \* Order of the block hashes, by id
    InitBalance,    \* Balance of every account before the modelled blocks
    InitRep,        \* Representative of every account before the modelled blocks
    Owner,          \* How the owner of every account behaves: "honest", "optional", "careless" or "byzantine"
    InitPublished,  \* Blocks released before the first step
    InitUp,         \* Nodes running at the start
    MaxTs,          \* Bound on the non-final votes a representative casts per root
    ByzNormalTs,    \* Timestamps of the non-final votes Byzantine representatives have on offer
    QuorumPercent,  \* online_reps::online_weight_quorum
    MinOnline,      \* node_config::online_weight_minimum
    InitTrended,    \* Trended online weight at the start
    DynamicQuorum,  \* Whether nodes track which representatives are online
    TrimLive,       \* Whether a node may drop a running representative from its online set
    StaleVotes,     \* Whether an election may outlive the online status of a representative it has a vote of
    Optimistic,     \* Whether elections start on account heads with uncemented dependencies
    Faults          \* Budget of disruptive events: [expire, crash, rollback, evict]

VARIABLES
    published,      \* Blocks released by their owners
    votes,          \* Every vote ever broadcast
    up,             \* Which nodes are running
    ledger,         \* Blocks in each node's ledger
    cemented,       \* Cemented blocks of each node
    forks,          \* Each node's fork cache
    finalVote,      \* Each node's final vote store, by root
    election,       \* Each node's election, by root
    finalReq,       \* Final votes each node's generator still has to process
    online,         \* Representatives each node counts as online
    trended,        \* Each node's trended online weight
    budget          \* Disruptive events left

vars == <<published, votes, up, ledger, cemented, forks, finalVote, election, finalReq, online, trended, budget>>

-----------------------------------------------------------------------------
(* Arithmetic *)

RECURSIVE SumOver(_, _)
SumOver(S, f) == IF S = {} THEN 0 ELSE LET x == CHOOSE y \in S : TRUE IN f[x] + SumOver(S \ {x}, f)

\* Sum of the w fields of a set of records
RECURSIVE SumW(_)
SumW(S) == IF S = {} THEN 0 ELSE LET x == CHOOSE y \in S : TRUE IN x.w + SumW(S \ {x})

Max(a, b) == IF a >= b THEN a ELSE b

-----------------------------------------------------------------------------
(* Blocks *)

NoBlock == "none"

Blocks == DOMAIN BlockDef
Accounts == DOMAIN InitBalance

Acct(b) == BlockDef[b].acct     \* Account the block belongs to
Prev(b) == BlockDef[b].prev     \* Previous block of the account, NoBlock for the first modelled one
Kind(b) == BlockDef[b].kind     \* "send", "receive" or "change"
Link(b) == BlockDef[b].link     \* Destination account of a send, source block of a receive
RepOf(b) == BlockDef[b].rep     \* Representative of the account after the block
BalOf(b) == BlockDef[b].bal     \* Balance of the account after the block

PrevBal(b) == IF Prev(b) = NoBlock THEN InitBalance[Acct(b)] ELSE BalOf(Prev(b))
Amount(b) == PrevBal(b) - BalOf(b)

\* Blocks competing for the same position share a root: nano::block::qualified_root
Root(b) == <<Acct(b), Prev(b)>>
Roots == {Root(b) : b \in Blocks}
RootOf == TLCEval([b \in Blocks |-> Root(b)])
ForksOf == TLCEval([r \in Roots |-> {b \in Blocks : Root(b) = r}])

\* nano::block::dependencies
Deps(b) == (IF Kind(b) = "receive" THEN {Prev(b), Link(b)} ELSE {Prev(b)}) \ {NoBlock}

RECURSIVE AncestorsOf(_)
AncestorsOf(b) == Deps(b) \cup UNION {AncestorsOf(d) : d \in Deps(b)}

\* The block with everything it depends on
Lineage == TLCEval([b \in Blocks |-> AncestorsOf(b) \cup {b}])

VotingReps == Voters \cup Byzantine
AllReps == Nodes \cup Byzantine \cup {InitRep[a] : a \in Accounts} \cup {RepOf(b) : b \in Blocks}

ASSUME
    /\ Voters \subseteq Nodes
    /\ InitUp \subseteq Nodes
    /\ Nodes \cap Byzantine = {}
    /\ NoBlock \notin Blocks
    /\ InitPublished \subseteq Blocks
    /\ DOMAIN HashRank = Blocks
    /\ \A a, b \in Blocks : HashRank[a] = HashRank[b] => a = b
    /\ DOMAIN InitRep = Accounts
    /\ DOMAIN Owner = Accounts
    /\ \A a \in Accounts : Owner[a] \in {"honest", "optional", "careless", "byzantine"}
    /\ \A b \in Blocks :
        /\ Acct(b) \in Accounts
        /\ Prev(b) = NoBlock \/ (Prev(b) \in Blocks /\ Acct(Prev(b)) = Acct(b))
        /\ Kind(b) \in {"send", "receive", "change"}
        /\ Kind(b) = "send" => Link(b) \in Accounts /\ BalOf(b) < PrevBal(b)
        /\ Kind(b) = "receive" =>
            /\ Link(b) \in Blocks
            /\ Kind(Link(b)) = "send"
            /\ Link(Link(b)) = Acct(b)
            /\ BalOf(b) = PrevBal(b) + Amount(Link(b))
        /\ Kind(b) = "change" => BalOf(b) = PrevBal(b)

-----------------------------------------------------------------------------
(* Ledger queries, L is the set of blocks in one ledger *)

\* The block the ledger holds at a root, as a set of at most one
At(L, r) == {b \in L : RootOf[b] = r}

\* The block with every ledger block built on it, what nano::ledger::rollback removes
Dependents(L, b) == {c \in L : b \in Lineage[c]}

HeadOf(L, a) == {b \in L : Acct(b) = a /\ \A c \in L : Prev(c) # b}

BalanceIn(L, a) == LET h == HeadOf(L, a) IN IF h = {} THEN InitBalance[a] ELSE BalOf(CHOOSE b \in h : TRUE)
RepIn(L, a) == LET h == HeadOf(L, a) IN IF h = {} THEN InitRep[a] ELSE RepOf(CHOOSE b \in h : TRUE)

\* Accounts without modelled blocks never move their weight
Movable == {Acct(b) : b \in Blocks}
StaticWeight == TLCEval([x \in AllReps |-> SumOver({a \in Accounts \ Movable : InitRep[a] = x}, InitBalance)])

\* nano::ledger::weight: follows every block in the ledger, cemented or not
Weight(L, x) == StaticWeight[x] + SumW({[acct |-> a, w |-> BalanceIn(L, a)] : a \in {m \in Movable : RepIn(L, m) = x}})

\* ledger_processor: the block extends the ledger
Insertable(L, b) ==
    /\ b \notin L
    /\ Deps(b) \subseteq L
    /\ At(L, RootOf[b]) = {}
    /\ Kind(b) = "receive" => \A c \in L : ~(Kind(c) = "receive" /\ Link(c) = Link(b))

-----------------------------------------------------------------------------
(* Votes *)

\* nano::vote::timestamp_final, above every non-final timestamp
FinalTs == MaxTs + 1

Vote(x, h, t) == [rep |-> x, hash |-> h, ts |-> t]

VoteType == [rep : VotingReps, hash : Blocks, ts : 1..FinalTs]

\* nano::vote_key: a representative's vote only advances by timestamp, then by hash
Supersedes(v, u) == u.ts < v.ts \/ (u.ts = v.ts /\ HashRank[u.hash] < HashRank[v.hash])

\* A Byzantine representative has every vote on offer from the start
ByzVotes == {Vote(x, h, t) : x \in Byzantine, h \in Blocks, t \in ByzNormalTs \cup {FinalTs}}

FinalVotesOf(x, r) == {v.hash : v \in {u \in votes : u.rep = x /\ u.ts = FinalTs /\ RootOf[u.hash] = r}}

\* Non-final votes a voter cast at a root so far, their timestamps count up from one
OwnNormal(n, r) == {v \in votes : v.rep = n /\ v.ts # FinalTs /\ RootOf[v.hash] = r}

-----------------------------------------------------------------------------
(* Quorum *)

\* online_reps::delta for a node that counts the representatives O as online
OnlineWeightOf(n, O) == SumW({[rep |-> x, w |-> Weight(ledger[n], x)] : x \in O})

DeltaOf(n, O) == (Max(Max(OnlineWeightOf(n, O), trended[n]), MinOnline) * QuorumPercent) \div 100

Delta(n) == DeltaOf(n, online[n])

-----------------------------------------------------------------------------
(* Elections *)

Idle == [state |-> "idle", winner |-> NoBlock, blocks |-> {}, ballot |-> {}]

\* election_ballot::evaluate: ballot B and the winner so far, weighed by the ledger of node n against delta
TallyOf(n, B, winnerSoFar, delta) ==
    LET \* Every recorded vote with the weight of its representative
        cast == {[rep |-> v.rep, hash |-> v.hash, final |-> v.ts = FinalTs, w |-> Weight(ledger[n], v.rep)] : v \in B}
        \* Weight behind every voted for block, and how much of it is final
        tallies == {[hash |-> h,
                     w |-> SumW({c \in cast : c.hash = h}),
                     final |-> SumW({c \in cast : c.hash = h /\ c.final})] : h \in {c.hash : c \in cast}}
        heaviest == CHOOSE p \in tallies : \A q \in tallies :
                        q.w < p.w \/ (q.w = p.w /\ HashRank[q.hash] <= HashRank[p.hash])
        \* The winner moves only once enough weight took part
        winner == IF tallies # {} /\ SumW(tallies) >= delta THEN heaviest.hash ELSE winnerSoFar
        own == {p \in tallies : p.hash = winner}
        rivals == tallies \ own
        winnerWeight == SumW(own)
        winnerFinal == IF own = {} THEN 0 ELSE (CHOOSE p \in own : TRUE).final
        runnerUp == IF rivals = {} THEN 0 ELSE (CHOOSE p \in rivals : \A q \in rivals : q.w <= p.w).w
    IN  [winner |-> winner,
         \* The winner leads every rival by the full delta
         quorum |-> winnerWeight >= runnerUp /\ winnerWeight - runnerUp >= delta,
         \* A full delta of final votes stands behind the winner
         final |-> winnerFinal >= delta]

Tally(n, r) == TallyOf(n, election[n][r].ballot, election[n][r].winner, Delta(n))

(***************************************************************************)
(* What node n does once its election at root r stands as e against delta, *)
(* with `recorded` on record as its final vote for the root:               *)
(* election::confirm_if_quorum, together with what it hands to the block   *)
(* processor and the cementing set, which run to completion in the same    *)
(* step.  A sealed election keeps its winner and only waits for it to      *)
(* reach the ledger.                                                       *)
(***************************************************************************)
React(n, r, e, delta, recorded) ==
    LET sealed == e.state = "confirmed"
        t == TallyOf(n, e.ballot, e.winner, delta)
        w == IF sealed THEN e.winner ELSE t.winner
        decided == sealed \/ (t.quorum /\ t.final)
        \* block_processor::force and rollback_competitor: the ledger follows the winner once it can take the place
        \* of the block held at the root, a cemented block is never rolled back
        competing == IF w \in ledger[n] THEN {} ELSE UNION {Dependents(ledger[n], c) : c \in At(ledger[n], r)}
        lands == /\ w \notin ledger[n]
                 /\ competing \cap cemented[n] = {}
                 /\ Insertable(ledger[n] \ competing, w)
        rolled == IF lands THEN competing ELSE {}
        newLedger == IF lands THEN (ledger[n] \ competing) \cup {w} ELSE ledger[n]
        \* cementing_set::run_batch and ledger::cement: a decided winner is cemented with all it depends on
        fresh == IF decided /\ w \in newLedger THEN Lineage[w] \ cemented[n] ELSE {}
        \* vote_generator::vote_final is asked for the winner at quorum, a request that cannot pass is left out
        requested == /\ n \in Voters
                     /\ sealed \/ t.quorum
                     /\ recorded = NoBlock
                     /\ w \in newLedger
                     /\ Deps(w) \subseteq cemented[n] \cup fresh
    IN  [ledger |-> newLedger,
         forks |-> IF lands THEN forks[n] \cup {w} ELSE forks[n],
         cemented |-> cemented[n] \cup fresh,
         election |-> [q \in Roots |->
            IF q = r THEN
                IF fresh # {} THEN Idle
                ELSE [e EXCEPT !.winner = w, !.state = IF decided THEN "confirmed" ELSE "active"]
            \* active_elections::block_cemented ends the elections of everything cemented
            ELSE IF At(fresh, q) # {} THEN Idle
            \* active_elections drops the elections of the other rolled back blocks
            ELSE IF election[n][q].state = "active" /\ At(rolled, q) # {} THEN Idle
            ELSE election[n][q]],
         \* vote_generator_index::push: a newer request replaces the one queued for the root
         finalReq |-> IF requested THEN (finalReq[n] \ ForksOf[r]) \cup {w} ELSE finalReq[n]]

\* active_elections::insert: the election starts on the ledger block and learns the cached forks
Started(n, b) == [state |-> "active", winner |-> b, blocks |-> {b} \cup (forks[n] \cap ForksOf[RootOf[b]]), ballot |-> {}]

\* Final vote weight the node may have cached for a block
CachedFinalWeight(n, b) ==
    SumW({[rep |-> v.rep, w |-> Weight(ledger[n], v.rep)] : v \in {u \in votes : u.hash = b /\ u.ts = FinalTs}})

\* When a scheduler starts an election for a ledger block
Activatable(n, b) ==
    \* scheduler::priority::activate, and scheduler::hinted with the dependency check
    \/ Deps(b) \subseteq cemented[n]
    \* scheduler::hinted::run_iterative: a cached final tally at quorum skips the dependency check
    \/ CachedFinalWeight(n, b) >= Delta(n)
    \* scheduler::optimistic::run_one
    \/ Optimistic /\ HeadOf(ledger[n], Acct(b)) = {b}

-----------------------------------------------------------------------------
(* Account owners *)

(***************************************************************************)
(* An owner signs and releases a block.  A byzantine owner signs anything  *)
(* at any time.  The others never sign two blocks at one root; a careless  *)
(* one receives a send before it is cemented, an honest and an optional    *)
(* one only after.  An honest owner releases every block it can, an        *)
(* optional one may wait forever.                                          *)
(***************************************************************************)
Publish(b) ==
    /\ b \notin published
    /\ Prev(b) = NoBlock \/ Prev(b) \in published
    /\ \/ Owner[Acct(b)] = "byzantine"
       \/ /\ Owner[Acct(b)] # "byzantine"
          /\ ForksOf[RootOf[b]] \cap published = {}
          /\ Kind(b) = "receive" =>
                IF Owner[Acct(b)] = "careless"
                THEN Link(b) \in published
                ELSE \E n \in Nodes : Link(b) \in cemented[n]
    /\ published' = published \cup {b}
    /\ UNCHANGED <<votes, up, ledger, cemented, forks, finalVote, election, finalReq, online, trended, budget>>

-----------------------------------------------------------------------------
(* Blocks arriving at a node *)

\* block_processor::process_one, and active_elections::handle_fork for a block that lost the race to its root
ReceiveBlock(n, b) ==
    LET r == RootOf[b] IN
    /\ up[n]
    /\ b \in published
    /\ b \notin ledger[n]
    /\ \/ /\ Insertable(ledger[n], b)
          /\ ledger' = [ledger EXCEPT ![n] = @ \cup {b}]
          \* The schedulers pick the block up at once, the newest block of an account is its head
          /\ election' = IF election[n][r].state = "idle" /\ (Optimistic \/ Activatable(n, b))
                         THEN [election EXCEPT ![n][r] = Started(n, b)]
                         ELSE election
          /\ UNCHANGED forks
       \/ /\ At(ledger[n], r) # {}
          /\ b \notin forks[n]
          /\ forks' = [forks EXCEPT ![n] = @ \cup {b}]
          \* election::publish, a sealed election admits no blocks
          /\ election' = IF election[n][r].state = "active"
                         THEN [election EXCEPT ![n][r].blocks = @ \cup {b}]
                         ELSE election
          /\ UNCHANGED ledger
    /\ UNCHANGED <<published, votes, up, cemented, finalVote, finalReq, online, trended, budget>>

-----------------------------------------------------------------------------
(* Elections *)

\* A scheduler starts the election of a ledger block later: after it expired, or once the block became eligible
StartElection(n, b) ==
    LET r == RootOf[b] IN
    /\ up[n]
    /\ b \in ledger[n] \ cemented[n]
    /\ election[n][r].state = "idle"
    /\ Activatable(n, b)
    /\ election' = [election EXCEPT ![n][r] = Started(n, b)]
    /\ UNCHANGED <<published, votes, up, ledger, cemented, forks, finalVote, finalReq, online, trended, budget>>

\* vote_router::vote and election::vote: a vote for a held block that advances the representative's vote is recorded and tallied
ReceiveVote(n, v) ==
    LET r == RootOf[v.hash]
        e == election[n][r]
        prior == {u \in e.ballot : u.rep = v.rep}
        \* online_reps::observe runs before the election checks its quorum
        heard == IF DynamicQuorum THEN online[n] \cup {v.rep} ELSE online[n]
        next == React(n, r, [e EXCEPT !.ballot = (@ \ prior) \cup {v}], DeltaOf(n, heard), finalVote[n][r])
    IN
    /\ up[n]
    /\ v \in votes
    /\ e.state = "active"
    /\ v.hash \in e.blocks
    /\ \A u \in prior : Supersedes(v, u)
    /\ ledger' = [ledger EXCEPT ![n] = next.ledger]
    /\ forks' = [forks EXCEPT ![n] = next.forks]
    /\ cemented' = [cemented EXCEPT ![n] = next.cemented]
    /\ election' = [election EXCEPT ![n] = next.election]
    /\ finalReq' = [finalReq EXCEPT ![n] = next.finalReq]
    /\ online' = [online EXCEPT ![n] = heard]
    /\ UNCHANGED <<published, votes, up, finalVote, trended, budget>>

\* election::tick and broadcast_vote_locked: the election is tallied again, which matters once weights, delta or the ledger moved under it
Reevaluate(n, r) ==
    LET e == election[n][r]
        next == React(n, r, e, Delta(n), finalVote[n][r])
    IN
    /\ up[n]
    /\ e.state # "idle"
    /\ \/ next.election # election[n]
       \/ next.ledger # ledger[n]
       \/ next.cemented # cemented[n]
       \/ next.finalReq # finalReq[n]
    /\ ledger' = [ledger EXCEPT ![n] = next.ledger]
    /\ forks' = [forks EXCEPT ![n] = next.forks]
    /\ cemented' = [cemented EXCEPT ![n] = next.cemented]
    /\ election' = [election EXCEPT ![n] = next.election]
    /\ finalReq' = [finalReq EXCEPT ![n] = next.finalReq]
    /\ UNCHANGED <<published, votes, up, finalVote, online, trended, budget>>

-----------------------------------------------------------------------------
(* Voting *)

\* The election wants a non-final vote for its winner that the voter has not cast yet
WantsNormalVote(n, r) ==
    LET e == election[n][r]
        mine == OwnNormal(n, r)
        t == Tally(n, r)
    IN
    /\ up[n]
    /\ n \in Voters
    /\ e.state = "active"
    \* vote_generator::process_normal and voting_policy::vote
    /\ e.winner \in ledger[n]
    /\ Deps(e.winner) \subseteq cemented[n]
    \* A recorded final vote turns every later vote into that final vote, which is out already
    /\ finalVote[n][r] = NoBlock
    /\ \A v \in mine : v.ts = Cardinality(mine) => v.hash # e.winner
    \* election::broadcast_vote_locked takes the final path at quorum.  The vote it asked for at its last tally is
    \* cast by vote_generator::process_normal although the weights may have moved since
    /\ ~t.quorum

\* vote_generator::broadcast_vote: the vote goes out and into the voter's own election in the same step
VoteNormal(n, r) ==
    LET e == election[n][r]
        v == Vote(n, e.winner, Cardinality(OwnNormal(n, r)) + 1)
        prior == {u \in e.ballot : u.rep = n}
        heard == IF DynamicQuorum THEN online[n] \cup {n} ELSE online[n]
        next == React(n, r, [e EXCEPT !.ballot = (@ \ prior) \cup {v}], DeltaOf(n, heard), finalVote[n][r])
    IN
    /\ WantsNormalVote(n, r)
    /\ Cardinality(OwnNormal(n, r)) < MaxTs
    /\ votes' = votes \cup {v}
    /\ ledger' = [ledger EXCEPT ![n] = next.ledger]
    /\ forks' = [forks EXCEPT ![n] = next.forks]
    /\ cemented' = [cemented EXCEPT ![n] = next.cemented]
    /\ election' = [election EXCEPT ![n] = next.election]
    /\ finalReq' = [finalReq EXCEPT ![n] = next.finalReq]
    /\ online' = [online EXCEPT ![n] = heard]
    /\ UNCHANGED <<published, up, finalVote, trended, budget>>

\* vote_generator::process_final and voting_policy::vote_final: the final vote slot of the root is claimed once
ProcessFinal(n, b) ==
    LET r == RootOf[b]
        e == election[n][r]
        v == Vote(n, b, FinalTs)
        prior == {u \in e.ballot : u.rep = n}
        heard == IF DynamicQuorum THEN online[n] \cup {n} ELSE online[n]
        next == React(n, r, [e EXCEPT !.ballot = (@ \ prior) \cup {v}], DeltaOf(n, heard), b)
    IN
    /\ up[n]
    /\ b \in finalReq[n]
    /\ IF b \in ledger[n] /\ Deps(b) \subseteq cemented[n] /\ finalVote[n][r] \in {NoBlock, b}
       THEN /\ finalVote' = [finalVote EXCEPT ![n][r] = b]
            /\ votes' = votes \cup {v}
            \* The vote goes out and into the voter's own election in the same step
            /\ IF e.state = "active" /\ b \in e.blocks
               THEN /\ ledger' = [ledger EXCEPT ![n] = next.ledger]
                    /\ forks' = [forks EXCEPT ![n] = next.forks]
                    /\ cemented' = [cemented EXCEPT ![n] = next.cemented]
                    /\ election' = [election EXCEPT ![n] = next.election]
                    /\ finalReq' = [finalReq EXCEPT ![n] = next.finalReq \ {b}]
                    /\ online' = [online EXCEPT ![n] = heard]
               ELSE /\ finalReq' = [finalReq EXCEPT ![n] = @ \ {b}]
                    /\ UNCHANGED <<ledger, cemented, forks, election, online>>
       ELSE /\ finalReq' = [finalReq EXCEPT ![n] = @ \ {b}]
            /\ UNCHANGED <<finalVote, votes, ledger, cemented, forks, election, online>>
    /\ UNCHANGED <<published, up, trended, budget>>

\* vote_replier::process and voting_policy::reply_final: a cemented block is answered with a final vote
\* A recorded final vote is answered with the recorded hash, which is out already
ReplyFinal(n, b) ==
    /\ up[n]
    /\ n \in Voters
    /\ b \in cemented[n]
    /\ finalVote[n][RootOf[b]] = NoBlock
    /\ Vote(n, b, FinalTs) \notin votes
    /\ votes' = votes \cup {Vote(n, b, FinalTs)}
    /\ UNCHANGED <<published, up, ledger, cemented, forks, finalVote, election, finalReq, online, trended, budget>>

-----------------------------------------------------------------------------
(* Disruptions, each spends from the budget *)

\* An election that times out or is cancelled for capacity loses its ballot
Expire(n, r) ==
    /\ up[n]
    /\ budget.expire > 0
    /\ election[n][r].state = "active"
    /\ election' = [election EXCEPT ![n][r] = Idle]
    /\ budget' = [budget EXCEPT !.expire = @ - 1]
    /\ UNCHANGED <<published, votes, up, ledger, cemented, forks, finalVote, finalReq, online, trended>>

\* fork_cache::put drops old entries to stay within its size
EvictFork(n, b) ==
    /\ up[n]
    /\ budget.evict > 0
    /\ b \in forks[n]
    /\ forks' = [forks EXCEPT ![n] = @ \ {b}]
    /\ budget' = [budget EXCEPT !.evict = @ - 1]
    /\ UNCHANGED <<published, votes, up, ledger, cemented, finalVote, election, finalReq, online, trended>>

\* bounded_backlog::perform_rollbacks: an uncemented block without an election is rolled back
Rollback(n, b) ==
    LET gone == Dependents(ledger[n], b) IN
    /\ up[n]
    /\ budget.rollback > 0
    /\ b \in ledger[n] \ cemented[n]
    /\ election[n][RootOf[b]].state = "idle"
    /\ ledger' = [ledger EXCEPT ![n] = @ \ gone]
    /\ election' = [election EXCEPT ![n] = [q \in Roots |->
            IF @[q].state = "active" /\ At(gone, q) # {} THEN Idle ELSE @[q]]]
    /\ budget' = [budget EXCEPT !.rollback = @ - 1]
    /\ UNCHANGED <<published, votes, up, cemented, forks, finalVote, finalReq, online, trended>>

\* A node stops: the ledger, the cemented blocks, the final votes and the trended weight are on disk, the rest is lost
Crash(n) ==
    /\ up[n]
    /\ budget.crash > 0
    /\ up' = [up EXCEPT ![n] = FALSE]
    /\ election' = [election EXCEPT ![n] = [q \in Roots |-> Idle]]
    /\ forks' = [forks EXCEPT ![n] = {}]
    /\ finalReq' = [finalReq EXCEPT ![n] = {}]
    /\ online' = [online EXCEPT ![n] = {}]
    /\ budget' = [budget EXCEPT !.crash = @ - 1]
    /\ UNCHANGED <<published, votes, ledger, cemented, finalVote, trended>>

Recover(n) ==
    /\ ~up[n]
    /\ up' = [up EXCEPT ![n] = TRUE]
    /\ UNCHANGED <<published, votes, ledger, cemented, forks, finalVote, election, finalReq, online, trended, budget>>

-----------------------------------------------------------------------------
(* Online weight *)

\* rep_crawler: a representative that answers a probe counts as online, a Byzantine one answers when it likes
Observe(n, x) ==
    /\ DynamicQuorum
    /\ up[n]
    /\ x \in VotingReps \ online[n]
    /\ x \in Nodes => up[x]
    /\ online' = [online EXCEPT ![n] = @ \cup {x}]
    /\ UNCHANGED <<published, votes, up, ledger, cemented, forks, finalVote, election, finalReq, trended, budget>>

\* online_reps::trim: a representative not heard from for a while stops counting, a Byzantine one falls silent when it likes
Trim(n, x) ==
    /\ DynamicQuorum
    /\ up[n]
    /\ x \in online[n]
    /\ TrimLive \/ (x \in Nodes => ~up[x])
    \* election::time_to_live is shorter than the wait, so no running election of the node still has a vote of it
    /\ StaleVotes \/ \A r \in Roots : election[n][r].state = "active" => \A v \in election[n][r].ballot : v.rep # x
    /\ online' = [online EXCEPT ![n] = @ \ {x}]
    /\ UNCHANGED <<published, votes, up, ledger, cemented, forks, finalVote, election, finalReq, trended, budget>>

\* online_reps::sample: the trended weight follows the online weight, samples under the minimum are skipped
Sample(n) ==
    LET onlineWeight == OnlineWeightOf(n, online[n]) IN
    /\ DynamicQuorum
    /\ up[n]
    /\ onlineWeight >= MinOnline
    /\ onlineWeight # trended[n]
    /\ trended' = [trended EXCEPT ![n] = onlineWeight]
    /\ UNCHANGED <<published, votes, up, ledger, cemented, forks, finalVote, election, finalReq, online, budget>>

-----------------------------------------------------------------------------
(* Specification *)

Init ==
    /\ published = InitPublished
    /\ votes = ByzVotes
    /\ up = [n \in Nodes |-> n \in InitUp]
    /\ ledger = [n \in Nodes |-> {}]
    /\ cemented = [n \in Nodes |-> {}]
    /\ forks = [n \in Nodes |-> {}]
    /\ finalVote = [n \in Nodes |-> [r \in Roots |-> NoBlock]]
    /\ election = [n \in Nodes |-> [r \in Roots |-> Idle]]
    /\ finalReq = [n \in Nodes |-> {}]
    /\ online = [n \in Nodes |-> IF DynamicQuorum THEN {x \in VotingReps : x \in Byzantine \/ x \in InitUp} ELSE {}]
    /\ trended = [n \in Nodes |-> InitTrended]
    /\ budget = Faults

\* An honest or careless owner releases what it can
PublishPromptly(b) == Owner[Acct(b)] \in {"honest", "careless"} /\ Publish(b)

\* An optional or byzantine owner releases when it likes
PublishAtWill(b) == Owner[Acct(b)] \in {"optional", "byzantine"} /\ Publish(b)

\* The vote of a voter reaches a node
ReceiveHonestVote(n, v) == v.rep \in Voters /\ ReceiveVote(n, v)

\* A Byzantine representative chooses who gets which of its votes
ReceiveByzantineVote(n, v) == v.rep \in Byzantine /\ ReceiveVote(n, v)

\* A stopped voter drops out of the online set
TrimStopped(n, x) == ~up[x] /\ Trim(n, x)

\* A running voter drops out of the online set of a node that no longer hears from it
TrimRunning(n, x) == up[x] /\ Trim(n, x)

\* Steps the nodes and the owners that keep going take on their own
Protocol ==
    \/ \E b \in Blocks : PublishPromptly(b)
    \/ \E n \in Nodes :
        \/ Recover(n)
        \/ Sample(n)
        \/ \E b \in Blocks : ReceiveBlock(n, b) \/ StartElection(n, b) \/ ProcessFinal(n, b) \/ ReplyFinal(n, b)
        \/ \E v \in VoteType : ReceiveHonestVote(n, v)
        \/ \E r \in Roots : Reevaluate(n, r) \/ VoteNormal(n, r)
        \/ \E x \in Voters : Observe(n, x) \/ TrimStopped(n, x)

\* Steps the adversary, the operators and the passing of time may take
Environment ==
    \/ \E b \in Blocks : PublishAtWill(b)
    \/ \E n \in Nodes :
        \/ Crash(n)
        \/ \E r \in Roots : Expire(n, r)
        \/ \E b \in Blocks : EvictFork(n, b) \/ Rollback(n, b)
        \/ \E v \in VoteType : ReceiveByzantineVote(n, v)
        \/ \E x \in Byzantine : Observe(n, x) \/ Trim(n, x)
        \/ \E x \in Voters : TrimRunning(n, x)

Next == Protocol \/ Environment

Spec == Init /\ [][Next]_vars

\* The nodes keep taking their own steps
FairSpec == Spec /\ WF_vars(Protocol)

-----------------------------------------------------------------------------
(* Invariants that must hold *)

ElectionType ==
    [state : {"idle", "active", "confirmed"},
     winner : Blocks \cup {NoBlock},
     blocks : SUBSET Blocks,
     ballot : SUBSET VoteType]

TypeOK ==
    /\ published \subseteq Blocks
    /\ votes \subseteq VoteType
    /\ up \in [Nodes -> BOOLEAN]
    /\ ledger \in [Nodes -> SUBSET Blocks]
    /\ cemented \in [Nodes -> SUBSET Blocks]
    /\ forks \in [Nodes -> SUBSET Blocks]
    /\ finalVote \in [Nodes -> [Roots -> Blocks \cup {NoBlock}]]
    /\ election \in [Nodes -> [Roots -> ElectionType]]
    /\ finalReq \in [Nodes -> SUBSET Blocks]
    /\ online \in [Nodes -> SUBSET VotingReps]
    /\ trended \in [Nodes -> Nat]
    /\ budget \in [expire : Nat, crash : Nat, rollback : Nat, evict : Nat]

\* Ledgers stay consistent and elections only hold what they were given
WellFormed ==
    \A n \in Nodes :
        /\ ledger[n] \subseteq published
        /\ \A b \in ledger[n] : Deps(b) \subseteq ledger[n]
        /\ \A b, c \in ledger[n] : RootOf[b] = RootOf[c] => b = c
        /\ cemented[n] \subseteq ledger[n]
        /\ \A b \in cemented[n] : Deps(b) \subseteq cemented[n]
        /\ \A r \in Roots :
            LET e == election[n][r] IN
            /\ e.state = "idle" => e = Idle
            /\ e.state # "idle" => e.winner \in e.blocks /\ e.blocks \subseteq ForksOf[r]
            /\ e.ballot \subseteq votes
            /\ \A v \in e.ballot : v.hash \in e.blocks
            /\ \A u, v \in e.ballot : u.rep = v.rep => u = v
            /\ finalVote[n][r] # NoBlock => Vote(n, finalVote[n][r], FinalTs) \in votes

\* No two nodes cement different blocks at one root
Agreement ==
    \A n, m \in Nodes : \A a \in cemented[n], b \in cemented[m] : RootOf[a] = RootOf[b] => a = b

\* A voter never issues final votes for two blocks at one root
FinalVotesUnique ==
    \A n \in Voters, r \in Roots : Cardinality(FinalVotesOf(n, r)) <= 1

-----------------------------------------------------------------------------
(* Conditions the scenarios probe *)

\* Voters never issue final votes for different blocks at one root
NoFinalVoteSplit ==
    \A r \in Roots : Cardinality(UNION {FinalVotesOf(n, r) : n \in Voters}) <= 1

\* Weight that can still end up behind a block at node n: every voter without a final vote for another block of its root
Attainable(n, b) ==
    SumW({[rep |-> x, w |-> Weight(ledger[n], x)] :
            x \in {y \in VotingReps : y \in Byzantine \/ FinalVotesOf(y, RootOf[b]) \subseteq {b}}})

\* At every root some block can still gather a delta of final votes
EveryRootWinnable ==
    \A n \in Nodes, r \in Roots : \E b \in ForksOf[r] : Attainable(n, b) >= Delta(n)

\* An election with a full delta of final votes behind its winner can confirm it
FinalQuorumConfirms ==
    \A n \in Nodes, r \in Roots :
        election[n][r].state = "active" => LET t == Tally(n, r) IN t.final => t.quorum

\* A block that can never be cemented at the node because a competitor of it, or of a block it builds on, was
Orphaned(n, b) == \E a \in Lineage[b] : \E c \in cemented[n] : RootOf[c] = RootOf[a] /\ c # a

Settled(n) == \A b \in published : b \in cemented[n] \/ Orphaned(n, b)

AllSettled == \A n \in Nodes : up[n] => Settled(n)

\* A voter is held back only by the bound on its votes
Exhausted == \E n \in Voters, r \in Roots : WantsNormalVote(n, r) /\ Cardinality(OwnNormal(n, r)) >= MaxTs

\* The nodes never run out of steps while a released block is undecided
NoDeadEnd == ENABLED Protocol \/ AllSettled \/ Exhausted

HonestSettled == \A n \in Nodes : up[n] => \A b \in published : Owner[Acct(b)] = "honest" => b \in cemented[n]

\* The same for the blocks of honest owners alone
HonestNeverStuck == ENABLED Protocol \/ HonestSettled \/ Exhausted

\* Every fair run decides every released block, unless the bound on votes cuts it short
EventuallySettled == <>[]AllSettled \/ <>Exhausted

=============================================================================
