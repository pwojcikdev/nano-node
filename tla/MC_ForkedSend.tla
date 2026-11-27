---------------------------- MODULE MC_ForkedSend ----------------------------
(***************************************************************************)
(* One account signs two sends at the same root, to different accounts.    *)
(* The destination of one of them receives it without waiting for it to be *)
(* cemented.  Three honest voters stay online.  The balance that is sent   *)
(* is delegated to a representative that does not vote, so neither send    *)
(* moves voting weight.  Elections also start on blocks whose dependencies *)
(* are not cemented, so the receive has one before its send is decided.    *)
(***************************************************************************)
EXTENDS NanoConsensus

CONSTANTS n1, n2, n3, idle

MC_BlockDef ==
    [SA |-> [acct |-> "x", prev |-> NoBlock, kind |-> "send", link |-> "y", rep |-> idle, bal |-> 0],
     SB |-> [acct |-> "x", prev |-> NoBlock, kind |-> "send", link |-> "q", rep |-> idle, bal |-> 0],
     RY |-> [acct |-> "y", prev |-> NoBlock, kind |-> "receive", link |-> "SA", rep |-> idle, bal |-> 1]]

MC_HashRank == [SA |-> 1, SB |-> 2, RY |-> 3]

MC_InitBalance == [x |-> 1, y |-> 0, q |-> 0, w1 |-> 34, w2 |-> 33, w3 |-> 33]
MC_InitRep == [x |-> idle, y |-> idle, q |-> idle, w1 |-> n1, w2 |-> n2, w3 |-> n3]
MC_Owner ==
    [x |-> "byzantine", y |-> "careless", q |-> "honest", w1 |-> "honest", w2 |-> "honest", w3 |-> "honest"]

MC_Faults == [expire |-> 0, crash |-> 0, rollback |-> 0, evict |-> 0]

\* The two voters of equal weight are interchangeable
MC_Symmetry == Permutations({n2, n3})

\* The receive of the uncemented send is decided at every node, one way or the other
MC_ReceiveSettles ==
    \/ ENABLED Protocol
    \/ Exhausted
    \/ "RY" \notin published
    \/ \A n \in Nodes : "RY" \in cemented[n] \/ Orphaned(n, "RY")

=============================================================================
