---------------------------- MODULE MC_Partition ----------------------------
(***************************************************************************)
(* One account signs two blocks at the same root while two voters of equal *)
(* weight may lose sight of each other.  Each node tracks which            *)
(* representatives are online and derives its quorum delta from that.      *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2]

MC_InitBalance == [x |-> 0, w1 |-> 50, w2 |-> 50]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

=============================================================================
