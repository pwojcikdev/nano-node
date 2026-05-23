------------------------------ MODULE ElectionBFT ------------------------------
(***************************************************************************)
(* Byzantine-fault model of Nano confirmation safety (ORV).                 *)
(*                                                                         *)
(* Unlike Election.tla (which checked the final-vote *mechanism* inside one  *)
(* honest election object), this spec asks the BFT question: how much       *)
(* adversarial STAKE defeats safety?  Representatives are partitioned into   *)
(* Correct and Byzantine, each with an integer Weight.  Confirmation of a    *)
(* block requires final-vote weight >= Quorum (the quorum delta             *)
(* Delta = 67% of online weight).                                           *)
(*                                                                         *)
(* Adversary model (authenticated Byzantine):                               *)
(*   - Correct reps obey the persisted final_votes barrier: each finalizes   *)
(*     AT MOST ONE block, ever.  Which block is chosen by the environment    *)
(*     (worst-case network/scheduling) -> a sound safety over-approximation. *)
(*   - Byzantine reps may finalize ANY number of conflicting blocks          *)
(*     (equivocate), but only on their own slot -> signatures unforgeable.   *)
(*                                                                         *)
(* TLC then DISCOVERS the threshold:                                        *)
(*   safety holds   iff   ByzWeight < 2*Quorum - TotalWeight  ( ~ 34% )      *)
(*   liveness holds iff   CorrectWeight >= Quorum             ( Byz <= 33% ) *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Reps,             \* set of (principal) representatives
    Byzantine,        \* faulty reps, Byzantine \subseteq Reps
    Weight,           \* [Reps -> Nat]  voting weight (stake)
    Blocks,           \* conflicting candidate blocks at one root (forks)
    Quorum,           \* quorum delta Delta (integer; models 67% of online weight)
    ByzantineActive   \* TRUE: commission faults (equivocate); FALSE: omission (withhold)

Correct == Reps \ Byzantine

ASSUME Byzantine \subseteq Reps
ASSUME Quorum \in Nat /\ Quorum > 0

RECURSIVE SumW(_)
SumW(S) == IF S = {} THEN 0
           ELSE LET r == CHOOSE x \in S : TRUE IN Weight[r] + SumW(S \ {r})

TotalWeight   == SumW(Reps)
ByzWeight     == SumW(Byzantine)
CorrectWeight == SumW(Correct)

VARIABLES
    final,      \* [Reps -> SUBSET Blocks]  blocks each rep has cast a FINAL vote for
    committed   \* SUBSET Blocks            blocks observed to have reached final quorum

vars == <<final, committed>>

\* Total final-vote weight behind a block (correct contributions + Byzantine).
FinalWeight(b) == SumW({ r \in Reps : b \in final[r] })
Committable(b) == FinalWeight(b) >= Quorum

Init ==
    /\ final = [r \in Reps |-> {}]
    /\ committed = {}

\* Correct rep finalizes a single block (barrier).  Block choice is the
\* environment's (models which fork the rep saw lead under adversarial timing).
CorrectFinal(r, b) ==
    /\ r \in Correct
    /\ final[r] = {}                         \* persisted final_votes barrier: once only
    /\ final' = [final EXCEPT ![r] = {b}]
    /\ UNCHANGED committed

\* Byzantine rep finalizes any block, possibly many (equivocation), on its own slot.
ByzFinal(r, b) ==
    /\ ByzantineActive
    /\ r \in Byzantine
    /\ final' = [final EXCEPT ![r] = @ \cup {b}]
    /\ UNCHANGED committed

\* Any observer commits b once it has seen a final-vote quorum for it.
Commit(b) ==
    /\ Committable(b)
    /\ committed' = committed \cup {b}
    /\ UNCHANGED final

Next ==
    \/ \E r \in Reps, b \in Blocks : CorrectFinal(r, b)
    \/ \E r \in Reps, b \in Blocks : ByzFinal(r, b)
    \/ \E b \in Blocks : Commit(b)
    \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
    /\ final \in [Reps -> SUBSET Blocks]
    /\ committed \subseteq Blocks

\* Sanity: the barrier is modeled (correct reps never finalize two blocks).
CorrectAtMostOneFinal == \A r \in Correct : Cardinality(final[r]) <= 1

\* *** BFT SAFETY CORE ***  At most one block can ever reach final quorum,
\* hence no two correct nodes can ever commit conflicting blocks.  Holds only
\* while ByzWeight < 2*Quorum - TotalWeight.
AtMostOneCommittable ==
    \A b1, b2 \in Blocks : (Committable(b1) /\ Committable(b2)) => b1 = b2

\* User-facing agreement: at most one decision among conflicting forks.
Agreement == Cardinality(committed) <= 1

\* Every recorded decision is backed by a real quorum.
DecisionSound == \A b \in committed : Committable(b)

\* Reachability / liveness probe.  On a healthy config this FAILS (a commit
\* trace exists); on a stalled config (Byz withholding, correct < Quorum) it
\* HOLDS, witnessing the liveness threshold.
NothingCommitted == committed = {}

THEOREM Safety == Spec => [](TypeOK /\ Agreement /\ AtMostOneCommittable /\ DecisionSound)

----------------------------------------------------------------------------
(* Concrete instance data, selected from each .cfg via  CONSTANT X <- ...    *)
(* (TLC .cfg cannot express function literals, so weights live here.)        *)

\* --- unit-weight family: W = 6, use Quorum = 4 (q = 2/3); boundary B >= 2 ---
RepsUnit   == {"c1", "c2", "c3", "c4", "z1", "z2"}
WeightUnit == [r \in RepsUnit |-> 1]
ByzOne     == {"z1"}          \* B = 1  (16.7%) -> safe
ByzTwo     == {"z1", "z2"}    \* B = 2  (33.3%) -> unsafe (= 2*Quorum - W)

\* --- weighted family: W = 100, Quorum = 67 (q = 0.67) to pin 33% vs 34% ---
RepsW      == {"c1", "c2", "z"}
Weight33   == ("c1" :> 34 @@ "c2" :> 33 @@ "z" :> 33)   \* ByzWeight 33 (33%)
Weight34   == ("c1" :> 33 @@ "c2" :> 33 @@ "z" :> 34)   \* ByzWeight 34 (34%)
ByzZ       == {"z"}
=============================================================================
