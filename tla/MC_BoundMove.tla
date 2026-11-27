---------------------------- MODULE MC_BoundMove ----------------------------
(***************************************************************************)
(* The aftermath of a final vote split, as in MC_Bound: one representative *)
(* has a final vote for block B on record, the one holding the remaining   *)
(* weight has its final vote for block A.  Every ledger holds A and every  *)
(* node knows both blocks.                                                 *)
(*                                                                         *)
(* An honest account then moves a balance from one representative to       *)
(* another with a change block.  The amount, the representative that loses *)
(* it and the one that gains it are set by the check; a balance moved to   *)
(* the fourth representative leaves the voters, because that one never     *)
(* votes.  The configuration runs the two bound voters; a check may add    *)
(* the third voter of MC_Bound, which has not voted yet.                   *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS big, rest, late, gone,
          BigWeight,    \* Weight of the representative bound to block B before the move, out of 100
          LateWeight,   \* Weight of the representative that has not voted yet
          Moved,        \* Balance that changes its representative
          From, To      \* Its representative before and after the change block

MC_BlockDef ==
    [A |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> big, bal |-> 0],
     B |-> [acct |-> "x", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> rest, bal |-> 0],
     D |-> [acct |-> "d", prev |-> NoBlock, kind |-> "change", link |-> NoBlock, rep |-> To, bal |-> Moved]]

MC_HashRank == [A |-> 1, B |-> 2, D |-> 3]

\* Weight of a representative that stays with it
MC_Staying(rep, weight) == IF From = rep THEN weight - Moved ELSE weight

MC_InitBalance ==
    [x |-> 0,
     d |-> Moved,
     w1 |-> MC_Staying(big, BigWeight),
     w2 |-> MC_Staying(rest, 100 - BigWeight - LateWeight),
     w3 |-> MC_Staying(late, LateWeight)]
MC_InitRep == [x |-> big, d |-> From, w1 |-> big, w2 |-> rest, w3 |-> late]
MC_Owner == [x |-> "byzantine", d |-> "honest", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* An election times out and the backlog rolls a block back
MC_FaultsRollback == [expire |-> 1, crash |-> 0, rollback |-> 1, evict |-> 0]

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

\* The nodes never run out of steps where one of them has cemented a block at the forked root and another has not
MC_CementedByAllOrNone ==
    \/ ENABLED Protocol
    \/ Exhausted
    \/ \A n, m \in Nodes : (cemented[n] \cap {"A", "B"} = {}) <=> (cemented[m] \cap {"A", "B"} = {})

=============================================================================
