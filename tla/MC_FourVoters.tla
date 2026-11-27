--------------------------- MODULE MC_FourVoters ---------------------------
(***************************************************************************)
(* One account signs two blocks at the same root.  Four honest voters of   *)
(* equal weight stay online; quorum takes three of them.                   *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, n3, n4

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2]

MC_InitBalance == [x |-> 0, w1 |-> 25, w2 |-> 25, w3 |-> 25, w4 |-> 25]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2, w3 |-> n3, w4 |-> n4]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest", w4 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* The voters are interchangeable
MC_Symmetry == Permutations({n1, n2, n3, n4})

=============================================================================
