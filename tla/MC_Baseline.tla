---------------------------- MODULE MC_Baseline ----------------------------
(***************************************************************************)
(* No forks.  One account sends to another, which receives the amount and  *)
(* then changes its representative, so the weight of all three voters      *)
(* moves while the blocks are being confirmed.  Every owner is honest.     *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, n3

MC_BlockDef ==
    [S |-> [acct |-> "a", prev |-> NoBlock, kind |-> "send", link |-> "b", rep |-> n1, bal |-> 0],
     R |-> [acct |-> "b", prev |-> NoBlock, kind |-> "receive", link |-> "S", rep |-> n2, bal |-> 10],
     C |-> [acct |-> "b", prev |-> "R", kind |-> "change", link |-> NoBlock, rep |-> n3, bal |-> 10]]

MC_HashRank == [S |-> 1, R |-> 2, C |-> 3]

MC_InitBalance == [a |-> 10, b |-> 0, w1 |-> 30, w2 |-> 30, w3 |-> 30]
MC_InitRep == [a |-> n1, b |-> n2, w1 |-> n1, w2 |-> n2, w3 |-> n3]
MC_Owner == [a |-> "honest", b |-> "honest", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* A node restarts
MC_FaultsCrash == [expire |-> 0, crash |-> 1, rollback |-> 0, evict |-> 0]

=============================================================================
