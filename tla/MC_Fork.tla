------------------------------ MODULE MC_Fork ------------------------------
(***************************************************************************)
(* One account signs two blocks at the same root.  Three honest voters     *)
(* hold all the weight and stay online.                                    *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, n3

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2]

MC_InitBalance == [x |-> 0, w1 |-> 34, w2 |-> 33, w3 |-> 33]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2, w3 |-> n3]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* An election times out and the backlog rolls a block back
MC_FaultsRollback == [expire |-> 1, crash |-> 0, rollback |-> 1, evict |-> 0]

\* An election times out, a node restarts and a fork drops out of a cache
MC_FaultsRestart == [expire |-> 1, crash |-> 1, rollback |-> 0, evict |-> 1]

\* The two voters of equal weight are interchangeable
MC_Symmetry == Permutations({n2, n3})

=============================================================================
