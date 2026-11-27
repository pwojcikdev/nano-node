----------------------------- MODULE MC_Pending -----------------------------
(***************************************************************************)
(* One account sends a large amount to another account, which has a        *)
(* different representative.  A node takes the amount away from the        *)
(* sender's representative as soon as the send is in its ledger, and gives *)
(* it to the receiver's representative only once the receive is there too. *)
(* In between the amount is delegated to nobody, while the quorum delta    *)
(* still follows the trended weight.                                       *)
(*                                                                         *)
(* The configuration has no fork.  A check may add a third account that    *)
(* signs two blocks at the same root.                                      *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2,
          Sent      \* Balance that is sent, out of 100

MC_BlockDef ==
    [S |-> [acct |-> "a", prev |-> NoBlock, kind |-> "send", link |-> "b", rep |-> n1, bal |-> 0],
     R |-> [acct |-> "b", prev |-> NoBlock, kind |-> "receive", link |-> "S", rep |-> n2, bal |-> Sent]]

MC_HashRank == [S |-> 1, R |-> 2]

\* The same next to a fork of another account
MC_WithFork ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n1, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0],
     S |-> MC_BlockDef.S,
     R |-> MC_BlockDef.R]

MC_WithForkRank == [A |-> 1, B |-> 2, S |-> 3, R |-> 4]

MC_InitBalance ==
    [x |-> 0,
     a |-> Sent,
     b |-> 0,
     w1 |-> (100 - Sent) - (100 - Sent) \div 2,
     w2 |-> (100 - Sent) \div 2]
MC_InitRep == [x |-> n1, a |-> n1, b |-> n2, w1 |-> n1, w2 |-> n2]
MC_Owner == [x |-> "byzantine", a |-> "honest", b |-> "honest", w1 |-> "honest", w2 |-> "honest"]

\* The receiver does not wait for the send to be cemented
MC_OwnerCareless == [x |-> "byzantine", a |-> "honest", b |-> "careless", w1 |-> "honest", w2 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

=============================================================================
