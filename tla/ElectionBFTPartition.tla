-------------------------- MODULE ElectionBFTPartition --------------------------
(***************************************************************************)
(* Synthesis model: per-node Nano elections under a Byzantine adversary AND *)
(* a partitionable network.  This is the union of the two earlier specs:    *)
(*                                                                         *)
(*   - Each honest NODE runs the full election state machine of Election.tla *)
(*     on its OWN local view of received votes: it tallies, applies the      *)
(*     winner-switch rule (switch to max-tally block once total tally >= Δ), *)
(*     and confirms on the two-threshold rule (margin >= Δ AND winner's      *)
(*     final-vote weight >= Δ).  No global final-vote set is assumed.        *)
(*                                                                         *)
(*   - A bounded-weight Byzantine set (ElectionBFT.tla) may equivocate:      *)
(*     send different (even FINAL) votes to different nodes.  Correct reps    *)
(*     obey the persisted final-vote barrier (one final block, consistent    *)
(*     across all nodes that see it).  No rep can forge another's vote.      *)
(*                                                                         *)
(*   - The NETWORK may deliver any vote to any subset of nodes (partition).  *)
(*     SyncDelivery = TRUE collapses this to one shared view (no partition). *)
(*                                                                         *)
(* Property checked: CrossNodeAgreement -- no two honest nodes confirm       *)
(* conflicting blocks.  TLC re-derives the same threshold (safe iff          *)
(* ByzWeight < 2*Quorum - TotalWeight) and shows the partition is the actual *)
(* attack vector: under SyncDelivery the attack disappears even at 34%.      *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Reps, Byzantine, Weight, Blocks, Quorum,
    Nodes,          \* honest observers, each running its own election
    InitBlock,      \* block every node's election opens with (initial winner)
    NoBlock,        \* sentinel: "this node has not confirmed"
    SyncDelivery    \* TRUE: every vote reaches all nodes (no partition)

Correct == Reps \ Byzantine

ASSUME Byzantine \subseteq Reps
ASSUME InitBlock \in Blocks
ASSUME NoBlock \notin Blocks
ASSUME Quorum \in Nat /\ Quorum > 0

RECURSIVE SumW(_)
SumW(S) == IF S = {} THEN 0
           ELSE LET r == CHOOSE x \in S : TRUE IN Weight[r] + SumW(S \ {r})

SetMax(S) == IF S = {} THEN 0 ELSE CHOOSE x \in S : \A y \in S : y <= x

TotalWeight == SumW(Reps)
ByzWeight   == SumW(Byzantine)

\* A per-rep vote slot in a node's view: <<kind, block>>, kind \in {"n","f","none"}
NoVote == <<"none", "none">>

VARIABLES
    view,     \* [Nodes -> [Reps -> vote slot]]  what each node has received
    winner,   \* [Nodes -> Blocks]               each node's current election winner
    state,    \* [Nodes -> {"active","confirmed"}]
    decided   \* [Nodes -> Blocks \cup {NoBlock}] each node's confirmed block

vars == <<view, winner, state, decided>>

\* Tallies over a single node's view  vw == view[n].
TallyN(vw, b)      == SumW({ r \in Reps : vw[r][2] = b /\ vw[r][1] \in {"n","f"} })
FinalTallyN(vw, b) == SumW({ r \in Reps : vw[r][1] = "f" /\ vw[r][2] = b })
TotalN(vw)         == SumW({ r \in Reps : vw[r][1] \in {"n","f"} })
ArgMaxN(vw)        == CHOOSE b \in Blocks : \A c \in Blocks : TallyN(vw,c) <= TallyN(vw,b)
SecondN(vw)        == SetMax({ TallyN(vw,c) : c \in (Blocks \ {ArgMaxN(vw)}) })
MarginN(vw)        == TallyN(vw, ArgMaxN(vw)) - SecondN(vw)

Init ==
    /\ view    = [n \in Nodes |-> [r \in Reps |-> NoVote]]
    /\ winner  = [n \in Nodes |-> InitBlock]
    /\ state   = [n \in Nodes |-> "active"]
    /\ decided = [n \in Nodes |-> NoBlock]

\* Can node n still receive votes?  Under sync, all nodes must be active
\* (one shared view advances together).
ActiveOK(n) == IF SyncDelivery THEN \A m \in Nodes : state[m] = "active"
                               ELSE state[n] = "active"

\* Deliver vote slot vt for rep r: to all nodes (sync) or just node n (async).
Deliver(n, r, vt) ==
    IF SyncDelivery
    THEN [m \in Nodes |-> [view[m] EXCEPT ![r] = vt]]
    ELSE [view EXCEPT ![n][r] = vt]

\* Barrier: no honest node may see correct rep r finalize a block other than b.
BarrierOK(r, b) == \A m \in Nodes : view[m][r][1] = "f" => view[m][r][2] = b

\* A node receives a NORMAL vote from r for b (any rep; normals may diverge).
RecvNormal(n, r, b) ==
    /\ ActiveOK(n)
    /\ view[n][r][1] # "f"                 \* monotonic: never downgrade a final
    /\ view' = Deliver(n, r, <<"n", b>>)
    /\ UNCHANGED <<winner, state, decided>>

\* A node receives a FINAL vote from r for b.  Correct reps are barrier-bound;
\* Byzantine reps may finalize anything and differently per node (equivocate).
RecvFinal(n, r, b) ==
    /\ ActiveOK(n)
    /\ \/ r \in Byzantine
       \/ (r \in Correct /\ BarrierOK(r, b))
    /\ view' = Deliver(n, r, <<"f", b>>)
    /\ UNCHANGED <<winner, state, decided>>

\* One node's election ticks: winner-switch then two-threshold confirm
\* (election::confirm_if_quorum, run on the node's local view).
Advance(n) ==
    /\ state[n] = "active"
    /\ LET vw    == view[n]
           cand  == ArgMaxN(vw)
           total == TotalN(vw)
           nw    == IF total >= Quorum /\ cand # winner[n] THEN cand ELSE winner[n]
           conf  == MarginN(vw) >= Quorum /\ FinalTallyN(vw, nw) >= Quorum
       IN  /\ winner'  = [winner  EXCEPT ![n] = nw]
           /\ state'   = [state   EXCEPT ![n] = IF conf THEN "confirmed" ELSE "active"]
           /\ decided' = [decided EXCEPT ![n] = IF conf THEN nw ELSE decided[n]]
    /\ UNCHANGED view

Next ==
    \/ \E n \in Nodes, r \in Reps, b \in Blocks : RecvNormal(n, r, b)
    \/ \E n \in Nodes, r \in Reps, b \in Blocks : RecvFinal(n, r, b)
    \/ \E n \in Nodes : Advance(n)
    \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
(* Invariants *)

VoteSlots == {NoVote} \cup { <<k, b>> : k \in {"n","f"}, b \in Blocks }

TypeOK ==
    /\ view    \in [Nodes -> [Reps -> VoteSlots]]
    /\ winner  \in [Nodes -> Blocks]
    /\ state   \in [Nodes -> {"active","confirmed"}]
    /\ decided \in [Nodes -> Blocks \cup {NoBlock}]

\* Sanity: the persisted barrier is honored by correct reps across all nodes.
BarrierConsistent ==
    \A r \in Correct, n, m \in Nodes :
        (view[n][r][1] = "f" /\ view[m][r][1] = "f") => view[n][r][2] = view[m][r][2]

\* A node only decides on a block its local view actually confirmed (frozen
\* after confirmation, so this stays true).
DecisionSound ==
    \A n \in Nodes :
        decided[n] # NoBlock =>
            /\ FinalTallyN(view[n], decided[n]) >= Quorum
            /\ MarginN(view[n]) >= Quorum

\* *** SAFETY: no two honest nodes confirm conflicting blocks. ***
CrossNodeAgreement ==
    \A n, m \in Nodes :
        (decided[n] # NoBlock /\ decided[m] # NoBlock) => decided[n] = decided[m]

\* Reachability probe: expected to FAIL on healthy configs (a node can decide).
NoDecision == \A n \in Nodes : decided[n] = NoBlock

THEOREM Safety == Spec => [](TypeOK /\ BarrierConsistent /\ DecisionSound /\ CrossNodeAgreement)

----------------------------------------------------------------------------
(* Concrete instance data, selected from each .cfg via  CONSTANT X <- ...    *)
(* Weighted family: W = 100, Quorum = 67 (q = 0.67), z = Byzantine.          *)

RepsW    == {"c1", "c2", "z"}
Weight33 == ("c1" :> 34 @@ "c2" :> 33 @@ "z" :> 33)   \* ByzWeight 33 (33%) -> safe
Weight34 == ("c1" :> 33 @@ "c2" :> 33 @@ "z" :> 34)   \* ByzWeight 34 (34%) -> unsafe
ByzZ     == {"z"}
=============================================================================
