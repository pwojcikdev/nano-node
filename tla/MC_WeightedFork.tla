--------------------------- MODULE MC_WeightedFork ---------------------------
(***************************************************************************)
(* One account signs two change blocks at the same root, each naming a     *)
(* different representative, and holds a balance of its own.  A node       *)
(* counts that balance for the representative named by the block its       *)
(* ledger holds, so the nodes weigh the votes of the election differently  *)
(* depending on which of the blocks they have, and a node's weights move   *)
(* when its election switches to the other block.                          *)
(*                                                                         *)
(* The voters hold W1, W2 and W3 of their own, the forked account holds    *)
(* the rest of 100.  The configuration runs two voters; a check adds the   *)
(* third or makes it Byzantine.                                            *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, n3,
          W1, W2, W3    \* Weights that do not move, out of 100

\* Balance of the account that signs the two blocks
MC_Stake == 100 - W1 - W2 - W3

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> MC_Stake],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> MC_Stake]]

MC_HashRank == [A |-> 1, B |-> 2]

MC_InitBalance == [x |-> MC_Stake, w1 |-> W1, w2 |-> W2, w3 |-> W3]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2, w3 |-> n3]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* A dead end comes only after a voter issued a final vote
MC_NoDeadEndWithoutFinal == NoDeadEnd \/ \E v \in votes : v.rep \in Voters /\ v.ts = FinalTs

=============================================================================
