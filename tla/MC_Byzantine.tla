---------------------------- MODULE MC_Byzantine ----------------------------
(***************************************************************************)
(* One account signs two blocks at the same root.  Two honest voters share *)
(* the weight a Byzantine representative does not hold.  The Byzantine     *)
(* representative has final and non-final votes for both blocks on offer   *)
(* and chooses which node gets which.                                      *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, z,
          ByzWeight     \* Weight of the Byzantine representative, out of 100

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2]

\* The same account without a second block at the root
MC_OneBlock == [A |-> MC_BlockDef.A]
MC_OneRank == [A |-> 1]

MC_InitBalance ==
    [x |-> 0,
     w1 |-> (100 - ByzWeight) - (100 - ByzWeight) \div 2,
     w2 |-> (100 - ByzWeight) \div 2,
     wz |-> ByzWeight]
MC_InitRep == [x |-> n1, w1 |-> n1, w2 |-> n2, wz |-> z]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest", wz |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* An election times out
MC_FaultsExpire == [expire |-> 1, crash |-> 0, rollback |-> 0, evict |-> 0]

=============================================================================
