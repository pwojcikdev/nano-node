-------------------------- MODULE MC_Redelegation --------------------------
(***************************************************************************)
(* One account signs two blocks at the same root while the owner of a      *)
(* large balance moves it from one voter to the other.  A node counts the  *)
(* balance for the new representative as soon as the change block is in    *)
(* its ledger, cemented or not, so the two voters can disagree about their *)
(* own weights.                                                            *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2,
          Stake     \* Balance that changes its representative, out of 100

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0],
     D |-> [acct |-> "p", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> Stake]]

MC_HashRank == [A |-> 1, B |-> 2, D |-> 3]

MC_InitBalance ==
    [x |-> 0,
     p |-> Stake,
     w1 |-> (100 - Stake) - (100 - Stake) \div 2,
     w2 |-> (100 - Stake) \div 2]
MC_InitRep == [x |-> n1, p |-> n1, w1 |-> n1, w2 |-> n2]
MC_Owner == [x |-> "byzantine", p |-> "byzantine", w1 |-> "honest", w2 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

=============================================================================
