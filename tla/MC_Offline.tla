----------------------------- MODULE MC_Offline -----------------------------
(***************************************************************************)
(* No forks.  40 of 100 weight is delegated to a representative that never *)
(* votes, while the trended weight of every node still counts it.  The two *)
(* voters left track which representatives are online.                     *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, absent

MC_BlockDef ==
    [H |-> [acct |-> "h", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0]]

MC_HashRank == [H |-> 1]

MC_InitBalance == [h |-> 0, w1 |-> 30, w2 |-> 30, idle |-> 40]
MC_InitRep == [h |-> n1, w1 |-> n1, w2 |-> n2, idle |-> absent]
MC_Owner == [h |-> "honest", w1 |-> "honest", w2 |-> "honest", idle |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

=============================================================================
