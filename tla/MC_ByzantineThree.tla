-------------------------- MODULE MC_ByzantineThree --------------------------
(***************************************************************************)
(* One account signs two blocks at the same root.  Three honest voters     *)
(* share the weight a Byzantine representative does not hold.  The         *)
(* Byzantine representative has final and non-final votes for both blocks  *)
(* on offer and chooses which node gets which.  Balances are in half       *)
(* percents of the total, which lets the two lighter voters weigh the      *)
(* same.                                                                   *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, n3, z,
          ByzWeight     \* Weight of the Byzantine representative, in percent

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> z, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2]

\* The two lighter voters hold 33 percent between them, the heaviest one the rest of the honest weight
MC_InitBalance ==
    [x |-> 0,
     w1 |-> 200 - 2 * ByzWeight - 66,
     w2 |-> 33,
     w3 |-> 33,
     wz |-> 2 * ByzWeight]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2, w3 |-> n3, wz |-> z]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest", wz |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* The two voters of equal weight are interchangeable
MC_Symmetry == Permutations({n2, n3})

=============================================================================
