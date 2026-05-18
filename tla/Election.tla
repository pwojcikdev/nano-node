-------------------------------- MODULE Election --------------------------------
(***************************************************************************)
(* Clean-room TLA+ model of the consensus decision implemented by the      *)
(* nano::election class (see doc/election-consensus-spec.md).               *)
(*                                                                         *)
(* It models ONE election (one chain position / qualified root) and the    *)
(* core loop `election::vote` -> `confirm_if_quorum`:                       *)
(*                                                                         *)
(*   - each representative has at most one current vote (normal or final)   *)
(*   - tally(b)      = sum of weights of reps whose current vote is b        *)
(*   - finalTally(b) = sum of weights of reps whose current FINAL vote is b  *)
(*   - winner switches to the max-tally block once total tally >= Quorum     *)
(*   - HaveQuorum := top1Tally - top2Tally >= Quorum   (the *margin* rule)   *)
(*   - confirm    := HaveQuorum  /\  finalTally(winner) >= Quorum            *)
(*   - confirm is one-way (state stays "confirmed", no further votes)        *)
(*                                                                         *)
(* The election class itself does NOT prevent a representative from         *)
(* finalizing two different blocks; that barrier is the persisted           *)
(* final_votes record in voting_policy.  We model it with `finalEver` and   *)
(* the AllowEquivocation flag so the barrier's safety role is checkable:    *)
(* with AllowEquivocation = FALSE the safety invariant holds; with TRUE     *)
(* TLC finds a counterexample (two blocks both reach final quorum).         *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Reps,              \* set of (principal) representatives
    Blocks,            \* set of competing blocks (forks) at this root
    Quorum,            \* quorum delta  (Delta = 67% of effective online weight)
    InitBlock,         \* block the election is opened with (initial winner)
    NoBlock,           \* sentinel distinct from every block ("no current vote")
    AllowEquivocation  \* TRUE => a rep may cast FINAL votes for >1 block

ASSUME InitBlock \in Blocks
ASSUME NoBlock \notin Blocks
ASSUME Quorum \in Nat /\ Quorum > 0

\* Uniform unit weight; referenced from the .cfg via  CONSTANT Weight <- UniformWeight
UniformWeight == [r \in Reps |-> 1]
CONSTANT Weight

VARIABLES
    voteHash,   \* [Reps -> Blocks \cup {NoBlock}]  current vote target
    voteFinal,  \* [Reps -> BOOLEAN]                current vote is final
    finalEver,  \* [Reps -> SUBSET Blocks]          every block ever finalized by r
    winner,     \* Blocks \cup {NoBlock}            status.winner
    state,      \* {"active","confirmed"}           election state (collapsed)
    decided     \* Blocks \cup {NoBlock}            block this election confirmed

vars == <<voteHash, voteFinal, finalEver, winner, state, decided>>

----------------------------------------------------------------------------
(* Weighted sums over a set of reps. *)
RECURSIVE SumW(_)
SumW(S) == IF S = {} THEN 0
           ELSE LET r == CHOOSE x \in S : TRUE IN Weight[r] + SumW(S \ {r})

SetMax(S) == IF S = {} THEN 0 ELSE CHOOSE x \in S : \A y \in S : y <= x

TallyOf(vh, b)          == SumW({ r \in Reps : vh[r] = b })
FinalTallyOf(vh, vf, b) == SumW({ r \in Reps : vh[r] = b /\ vf[r] })
TotalOf(vh)             == SumW({ r \in Reps : vh[r] \in Blocks })

\* The block with the greatest tally (CHOOSE breaks ties, mirroring the
\* arbitrary ordering of equal-weight entries in the C++ tally map).
ArgMaxOf(vh) == CHOOSE b \in Blocks : \A c \in Blocks : TallyOf(vh,c) <= TallyOf(vh,b)

\* tally of the runner-up (0 if only one block has weight / one block total).
SecondOf(vh) == LET w == ArgMaxOf(vh)
                IN  SetMax({ TallyOf(vh,c) : c \in (Blocks \ {w}) })

\* election::have_quorum  ->  T1 - T2 >= Delta   (absolute top two, winner-independent)
HaveQuorumPost(vh) == TallyOf(vh, ArgMaxOf(vh)) - SecondOf(vh) >= Quorum

\* Weighted count of reps that have *ever* cast a final vote for b.
EverFinalTally(b) == SumW({ r \in Reps : b \in finalEver[r] })

----------------------------------------------------------------------------
Init ==
    /\ voteHash  = [r \in Reps |-> NoBlock]
    /\ voteFinal = [r \in Reps |-> FALSE]
    /\ finalEver = [r \in Reps |-> {}]
    /\ winner    = InitBlock
    /\ state     = "active"
    /\ decided   = NoBlock

(* Shared post-vote step: apply election::confirm_if_quorum using the *)
(* already-primed vote functions newVH / newVF.                        *)
ApplyConfirm(newVH, newVF) ==
    LET cand      == ArgMaxOf(newVH)
        total     == TotalOf(newVH)
        \* winner only flips once total tally reaches quorum (anti-flap guard)
        newWinner == IF total >= Quorum /\ cand # winner THEN cand ELSE winner
        doConfirm == /\ state = "active"
                     /\ HaveQuorumPost(newVH)
                     /\ FinalTallyOf(newVH, newVF, newWinner) >= Quorum
    IN  /\ winner'  = newWinner
        /\ state'   = IF doConfirm THEN "confirmed" ELSE state
        /\ decided' = IF doConfirm /\ decided = NoBlock THEN newWinner ELSE decided

(* election::vote -- a representative (re)casts a NORMAL vote for b.        *)
(* Allowing free changes among normal votes over-approximates timestamp    *)
(* monotonicity: it gives the environment strictly more power, so any      *)
(* safety property proved here also holds under the real (stricter) rule.  *)
CastNormal(r, b) ==
    /\ state = "active"
    /\ ~voteFinal[r]                       \* a final vote is frozen (monotonic)
    /\ LET newVH == [voteHash  EXCEPT ![r] = b]
           newVF == [voteFinal EXCEPT ![r] = FALSE]
       IN  /\ voteHash'  = newVH
           /\ voteFinal' = newVF
           /\ UNCHANGED finalEver
           /\ ApplyConfirm(newVH, newVF)

(* election::vote -- a representative casts a FINAL vote for b.            *)
(* Honest reps finalize at most one block ever (the persisted final_votes  *)
(* barrier).  With AllowEquivocation a rep may re-finalize a different     *)
(* block, which the election class alone cannot prevent.                   *)
CastFinal(r, b) ==
    /\ state = "active"
    /\ \/ ~voteFinal[r]                    \* first finalization
       \/ AllowEquivocation                \* or equivocation explicitly enabled
    /\ LET newVH == [voteHash  EXCEPT ![r] = b]
           newVF == [voteFinal EXCEPT ![r] = TRUE]
       IN  /\ voteHash'  = newVH
           /\ voteFinal' = newVF
           /\ finalEver' = [finalEver EXCEPT ![r] = @ \cup {b}]
           /\ ApplyConfirm(newVH, newVF)

\* Harmless self-loop so confirmed / all-final states are not deadlocks.
Stutter == UNCHANGED vars

Next ==
    \/ \E r \in Reps, b \in Blocks : CastNormal(r, b)
    \/ \E r \in Reps, b \in Blocks : CastFinal(r, b)
    \/ Stutter

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
    /\ voteHash  \in [Reps -> Blocks \cup {NoBlock}]
    /\ voteFinal \in [Reps -> BOOLEAN]
    /\ finalEver \in [Reps -> SUBSET Blocks]
    /\ winner    \in Blocks \cup {NoBlock}
    /\ state     \in {"active", "confirmed"}
    /\ decided   \in Blocks \cup {NoBlock}

\* Confirmation is one-way: once decided it never changes and the election
\* is parked in "confirmed" with winner pinned to the decided block.
DecidedStable ==
    decided # NoBlock => (state = "confirmed" /\ winner = decided)

\* A confirmed block really did accumulate >= Quorum final-vote weight.
DecidedHadFinalQuorum ==
    decided # NoBlock => EverFinalTally(decided) >= Quorum

\* *** Core safety theorem ***
\* At most one block can EVER reach final-vote quorum.  This is what makes
\* the second confirmation threshold a safe one-way ratchet across nodes:
\* every node that confirms necessarily confirms the same block.  It holds
\* only because honest reps never finalize two blocks (no equivocation).
SafeUniqueFinal ==
    \A b1, b2 \in Blocks :
        (EverFinalTally(b1) >= Quorum /\ EverFinalTally(b2) >= Quorum) => b1 = b2

\* Reachability probe (used by the "live" cfg): expected to FAIL, proving
\* the model can actually confirm and the safety result is not vacuous.
NotConfirmed == decided = NoBlock

THEOREM Safety == Spec => [](TypeOK /\ DecidedStable /\ DecidedHadFinalQuorum /\ SafeUniqueFinal)
=============================================================================
