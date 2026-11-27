--------------------------- MODULE MC_Successors ---------------------------
(***************************************************************************)
(* One account signs two blocks at the same root and one more block on top *)
(* of each.  Two honest voters of unequal weight hold all the weight.      *)
(* Elections also start on blocks whose dependencies are not cemented, so  *)
(* a rollback takes blocks with elections along.                           *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2

MC_BlockDef ==
    [A  |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B  |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0],
     A2 |-> [acct |-> "x", prev |-> "A", kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0],
     B2 |-> [acct |-> "x", prev |-> "B", kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2, A2 |-> 3, B2 |-> 4]

MC_InitBalance == [x |-> 0, w1 |-> 60, w2 |-> 40]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* An election times out and the backlog rolls a block back
MC_FaultsRollback == [expire |-> 1, crash |-> 0, rollback |-> 1, evict |-> 0]

=============================================================================
