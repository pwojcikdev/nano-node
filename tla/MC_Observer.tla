---------------------------- MODULE MC_Observer ----------------------------
(***************************************************************************)
(* One account signs two blocks at the same root.  Two honest voters of    *)
(* unequal weight hold all the weight, a third node does not vote and only *)
(* follows.  Neither voter reaches quorum without the other, and the       *)
(* lighter one follows the heavier one.                                    *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, watcher

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2]

MC_InitBalance == [x |-> 0, w1 |-> 60, w2 |-> 40]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* An election times out, a node restarts and a fork drops out of a cache
MC_FaultsRestart == [expire |-> 1, crash |-> 1, rollback |-> 0, evict |-> 1]

\* An election times out and the backlog rolls a block back
MC_FaultsRollback == [expire |-> 1, crash |-> 0, rollback |-> 1, evict |-> 0]

\* One disruption of every kind
MC_FaultsAll == [expire |-> 1, crash |-> 1, rollback |-> 1, evict |-> 1]

=============================================================================
