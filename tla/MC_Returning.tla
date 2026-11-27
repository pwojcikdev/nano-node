---------------------------- MODULE MC_Returning ----------------------------
(***************************************************************************)
(* The aftermath of a final vote split, with voting weight that was        *)
(* offline and comes back.  One representative has its final vote on       *)
(* record for block B and the one holding most of the remaining weight has *)
(* its final vote for block A.  A third one, holding 15, was offline all   *)
(* along: it has neither block and no vote, and the other two do not count *)
(* it as online.  The two running nodes hold A and know both blocks.       *)
(*                                                                         *)
(* With online weight tracking on, the two running nodes take their quorum *)
(* delta from the 85 they see, and it rises once the third one is back.    *)
(* With tracking off, delta stays at 67% of 85.                            *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS bound, rest, away,
          BoundWeight   \* Weight of the representative bound to block B, out of 100

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> bound, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> rest, bal |-> 0]]

MC_HashRank == [A |-> 1, B |-> 2]

MC_InitBalance == [x |-> 0, w1 |-> BoundWeight, w2 |-> 85 - BoundWeight, w3 |-> 15]
MC_InitRep == [x |-> bound, w1 |-> bound, w2 |-> rest, w3 |-> away]
MC_Owner == [x |-> "byzantine", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

MC_Recorded == [n \in Nodes |-> IF n = bound THEN "B" ELSE IF n = rest THEN "A" ELSE NoBlock]

\* The running nodes see only each other and their trended weight has followed that; the returning one remembers all of the weight
MC_Init ==
    /\ published = {"A", "B"}
    /\ votes = {Vote(n, MC_Recorded[n], FinalTs) : n \in {bound, rest}}
    /\ up = [n \in Nodes |-> n # away]
    /\ ledger = [n \in Nodes |-> IF n = away THEN {} ELSE {"A"}]
    /\ cemented = [n \in Nodes |-> {}]
    /\ forks = [n \in Nodes |-> IF n = away THEN {} ELSE {"B"}]
    /\ finalVote = [n \in Nodes |-> [r \in Roots |-> MC_Recorded[n]]]
    /\ election = [n \in Nodes |-> [r \in Roots |-> Idle]]
    /\ finalReq = [n \in Nodes |-> {}]
    /\ online = [n \in Nodes |-> IF DynamicQuorum /\ n # away THEN {bound, rest} ELSE {}]
    /\ trended = [n \in Nodes |-> IF DynamicQuorum /\ n = away THEN 100 ELSE 85]
    /\ budget = Faults

MC_Spec == MC_Init /\ [][Next]_vars

MC_FairSpec == MC_Spec /\ WF_vars(Protocol)

\* No node ever cements a block at the forked root
MC_NeverCemented == \A n \in Nodes : cemented[n] = {}

=============================================================================
