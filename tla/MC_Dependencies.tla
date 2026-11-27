-------------------------- MODULE MC_Dependencies --------------------------
(***************************************************************************)
(* The accounts around a forked send that can no longer be confirmed.  One *)
(* account signed two sends at the same root and the two voters have their *)
(* final votes on record for different ones: the aftermath of a final vote *)
(* split, of the kind MC_Fork shows honest voters reach.                   *)
(*                                                                         *)
(* The destination of one send receives it without waiting for it to be    *)
(* cemented.  A third account, unrelated to both, changes its              *)
(* representative.                                                         *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2

MC_BlockDef ==
    [SA |-> [acct |-> "x", prev |-> NoBlock, kind |-> "send", link |-> "y", rep |-> n1, bal |-> 0],
     SB |-> [acct |-> "x", prev |-> NoBlock, kind |-> "send", link |-> "q", rep |-> n1, bal |-> 0],
     RY |-> [acct |-> "y", prev |-> NoBlock, kind |-> "receive", link |-> "SA", rep |-> n2, bal |-> 10],
     H  |-> [acct |-> "h", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> n2, bal |-> 0]]

MC_HashRank == [SA |-> 1, SB |-> 2, RY |-> 3, H |-> 4]

MC_InitBalance == [x |-> 10, y |-> 0, q |-> 0, h |-> 0, w1 |-> 40, w2 |-> 50]
MC_InitRep == [x |-> n1, y |-> n2, q |-> n1, h |-> n1, w1 |-> n1, w2 |-> n2]
MC_Owner == [x |-> "byzantine", y |-> "careless", q |-> "honest", h |-> "honest", w1 |-> "honest", w2 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

MC_Recorded == [n \in Nodes |-> IF n = n1 THEN "SA" ELSE "SB"]

MC_Init ==
    /\ published = {"SA", "SB"}
    /\ votes = {Vote(n, MC_Recorded[n], FinalTs) : n \in Nodes}
    /\ up = [n \in Nodes |-> TRUE]
    /\ ledger = [n \in Nodes |-> {MC_Recorded[n]}]
    /\ cemented = [n \in Nodes |-> {}]
    /\ forks = [n \in Nodes |-> {"SA", "SB"} \ {MC_Recorded[n]}]
    /\ finalVote = [n \in Nodes |-> [r \in Roots |-> IF r = RootOf["SA"] THEN MC_Recorded[n] ELSE NoBlock]]
    /\ election = [n \in Nodes |-> [r \in Roots |-> Idle]]
    /\ finalReq = [n \in Nodes |-> {}]
    /\ online = [n \in Nodes |-> {}]
    /\ trended = [n \in Nodes |-> InitTrended]
    /\ budget = Faults

MC_Spec == MC_Init /\ [][Next]_vars

\* The receive of the uncemented send is decided at every node, one way or the other
MC_ReceiveSettles ==
    \/ ENABLED Protocol
    \/ Exhausted
    \/ "RY" \notin published
    \/ \A n \in Nodes : "RY" \in cemented[n] \/ Orphaned(n, "RY")

=============================================================================
