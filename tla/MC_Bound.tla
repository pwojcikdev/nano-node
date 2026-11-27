------------------------------ MODULE MC_Bound ------------------------------
(***************************************************************************)
(* The aftermath of a final vote split, of the kind MC_Fork shows honest   *)
(* voters reach.  One large representative has a final vote for block B on *)
(* record, the representative holding most of the remaining weight has its *)
(* final vote for block A, a third one has not voted yet.  Every ledger    *)
(* holds A and every node knows both blocks.                               *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS big, rest, late,
          BigWeight     \* Weight of the representative bound to block B, out of 100

MC_Forks ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> big, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> rest, bal |-> 0]]

MC_ForkRank == [A |-> 1, B |-> 2]

MC_InitBalance == [x |-> 0, w1 |-> BigWeight, w2 |-> 90 - BigWeight, w3 |-> 10]
MC_InitRep == [x |-> big, w1 |-> big, w2 |-> rest, w3 |-> late]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* Elections time out, a node restarts and forks drop out of the caches
MC_FaultsRestart == [expire |-> 2, crash |-> 1, rollback |-> 0, evict |-> 2]

MC_Recorded == [n \in Nodes |-> IF n = big THEN "B" ELSE IF n = rest THEN "A" ELSE NoBlock]

MC_Init ==
    /\ published = {"A", "B"}
    /\ votes = {Vote(n, MC_Recorded[n], FinalTs) : n \in {big, rest}}
    /\ up = [n \in Nodes |-> TRUE]
    /\ ledger = [n \in Nodes |-> {"A"}]
    /\ cemented = [n \in Nodes |-> {}]
    /\ forks = [n \in Nodes |-> {"B"}]
    /\ finalVote = [n \in Nodes |-> [r \in Roots |-> IF r = RootOf["A"] THEN MC_Recorded[n] ELSE NoBlock]]
    /\ election = [n \in Nodes |-> [r \in Roots |-> Idle]]
    /\ finalReq = [n \in Nodes |-> {}]
    /\ online = [n \in Nodes |-> {}]
    /\ trended = [n \in Nodes |-> InitTrended]
    /\ budget = Faults

MC_Spec == MC_Init /\ [][Next]_vars

\* No node ever cements a block at the forked root
MC_NeverCemented == \A n \in Nodes : cemented[n] \cap {"A", "B"} = {}

=============================================================================
