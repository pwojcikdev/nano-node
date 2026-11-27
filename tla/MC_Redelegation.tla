-------------------------- MODULE MC_Redelegation --------------------------
(***************************************************************************)
(* One account signs two blocks at the same root while another account     *)
(* moves its balance from one representative to another with a change      *)
(* block.  A node that has the change block counts the balance for the new *)
(* representative, a node that has not counts it for the old one, so two   *)
(* nodes can disagree about the weights while they tally the same votes.   *)
(*                                                                         *)
(* Two voters hold W1 and W2 of their own and a third representative holds *)
(* WOther; the balance that moves is the rest of 100.  The configuration   *)
(* moves it from the first voter to the second and leaves the third        *)
(* representative, which never votes, without weight.  A check may name    *)
(* the third one as the source or the target of the move.                  *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, other,
          W1, W2, WOther,   \* Weights that do not move, out of 100
          From, To          \* Representative of the balance that moves, before and after the change block

\* Balance that changes its representative
MC_Stake == 100 - W1 - W2 - WOther

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0],
     D |-> [acct |-> "p", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> To, bal |-> MC_Stake]]

MC_HashRank == [A |-> 1, B |-> 2, D |-> 3]

\* The change block alone, without a fork
MC_OnlyMove == [D |-> MC_BlockDef.D]
MC_OnlyMoveRank == [D |-> 1]

\* The change block without a fork, and a second change block on top of it that names the second voter
MC_MoveAndBack ==
    [D |-> MC_BlockDef.D,
     D2 |-> [acct |-> "p", prev |-> "D", kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> MC_Stake]]

MC_MoveAndBackRank == [D |-> 1, D2 |-> 2]

MC_InitBalance == [x |-> 0, p |-> MC_Stake, w1 |-> W1, w2 |-> W2, w3 |-> WOther]
MC_InitRep == [x |-> n1, p |-> From, w1 |-> n1, w2 |-> n2, w3 |-> other]

\* The owner of the balance signs its blocks once and releases them at once
MC_Owner == [x |-> "byzantine", p |-> "honest", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* An election times out and the backlog rolls a block back
MC_FaultsRollback == [expire |-> 1, crash |-> 0, rollback |-> 1, evict |-> 0]

\* The nodes never run out of steps where one of them has cemented a block at the forked root and another has not
MC_CementedByAllOrNone ==
    \/ ENABLED Protocol
    \/ Exhausted
    \/ \A n, m \in Nodes : (cemented[n] \cap {"A", "B"} = {}) <=> (cemented[m] \cap {"A", "B"} = {})

=============================================================================
