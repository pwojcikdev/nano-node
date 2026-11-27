---------------------------- MODULE MC_SecondFork ----------------------------
(***************************************************************************)
(* One account signs two blocks at the same root and then two more on top  *)
(* of one of them.  Three honest voters hold all the weight and stay       *)
(* online.  The second fork has its elections once the block under it is   *)
(* cemented, at each node in its own time.                                 *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, n3

MC_BlockDef ==
    [A  |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B  |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0],
     A2 |-> [acct |-> "x", prev |-> "A", kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0],
     A3 |-> [acct |-> "x", prev |-> "A", kind |-> "change", link |-> NoBlock, rep |-> n3, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2, A2 |-> 3, A3 |-> 4]

MC_InitBalance == [x |-> 0, w1 |-> 34, w2 |-> 33, w3 |-> 33]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2, w3 |-> n3]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* The two voters of equal weight are interchangeable
MC_Symmetry == Permutations({n2, n3})

=============================================================================
