------------------------ MODULE IntrusiveTaggedStack ------------------------
(***************************************************************************)
(* include/lockfree/intrusive_tagged_stack: a Treiber stack whose head is  *)
(* a (pointer, counter) word.  One label per shared-memory access:         *)
(*                                                                         *)
(*   push(node)   head.load; loop { store_link(node, head.ptr);            *)
(*                CAS head -> (node, counter + 1) }                        *)
(*   pop()        head.load; loop { if null return; next = load_link(cur); *)
(*                CAS head -> (next, counter + 1); on success              *)
(*                store_link(cur, null) }                                  *)
(*   steal_all()  CAS loop head -> (null, counter + 1)                     *)
(*                                                                         *)
(* A thread owns the nodes it popped or stole and may push them back       *)
(* (the allocator frees a block it allocated).  The counter wraps at       *)
(* TagMod (2^16 in C++); TagMod larger than the number of head changes in *)
(* a run means "no wrap during one stall", the documented assumption.     *)
(* Memory is type-stable (nodes are never unmapped): a stale load_link of *)
(* a node now owned elsewhere returns whatever link it holds.  With        *)
(* Scribble, owners may set that link to anything, so a stale popper can  *)
(* read any value; its CAS must still fail.                                *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    Nodes,          \* node ids, e.g. 1..3
    Threads,        \* thread ids, e.g. {11, 12}
    OpsPerThread,
    TagMod,
    UseSteal,       \* TRUE: threads may also steal_all
    Scribble        \* TRUE: an owner may overwrite the link word of a node it owns with any value (the static
                    \*   arena's bin-7 splice via store_link; the single arena reusing a metadata record)

NULL == 0
ASSUME NULL \notin Nodes

(* --algorithm IntrusiveTaggedStack {
variables
    headPtr = CHOOSE n \in Nodes : \A m \in Nodes : n <= m,     \* initially all nodes stacked in order
    headTag = 0,
    link = [n \in Nodes |-> IF n + 1 \in Nodes THEN n + 1 ELSE NULL],
    owned = [t \in Threads |-> {}];

define {
    \* Nodes reachable from head (the chain is followed at most |Nodes| + 1 steps; a longer chain is a cycle)
    RECURSIVE Chain(_, _)
    Chain(p, k) == IF p = NULL \/ k = 0 THEN <<>> ELSE <<p>> \o Chain(link[p], k - 1)
    StackSeq == Chain(headPtr, Cardinality(Nodes) + 1)
    OnStack == {StackSeq[i] : i \in 1..Len(StackSeq)}
    AllOwned == UNION {owned[t] : t \in Threads}

    NoCycle == Len(StackSeq) <= Cardinality(Nodes) /\ Cardinality(OnStack) = Len(StackSeq)
    \* A node is either on the stack or owned by exactly one thread: never both, never lost, never twice
    OwnedNotOnStack == AllOwned \cap OnStack = {}
    OwnedOnce == \A t1, t2 \in Threads : t1 # t2 => owned[t1] \cap owned[t2] = {}
}

fair process (T \in Threads)
variables ops = 0, cur = NULL, curTag = 0, nxt = NULL, pushing = NULL, stolen = <<>>;
{
 L_loop:
    while (ops < OpsPerThread) {
        ops := ops + 1;
        either {
            \* ---------------- pop
 P_load:    cur := headPtr; curTag := headTag;
 P_check:   if (cur = NULL) { goto L_next; };
 P_link:    nxt := link[cur];                              \* load_link: may be stale
 P_cas:     if (headPtr = cur /\ headTag = curTag) {
                headPtr := nxt; headTag := (curTag + 1) % TagMod;
                assert cur \notin AllOwned;                \* the popped node must not already be someone's
                owned[self] := owned[self] \cup {cur};
            } else {
                cur := headPtr; curTag := headTag;
                goto P_check;
            };
 P_clear:   link[cur] := NULL;                             \* store_link(cur, null)
        } or {
            \* ---------------- push one owned node
            await owned[self] # {};
            with (n \in owned[self]) { pushing := n; owned[self] := owned[self] \ {n}; };
 U_load:    cur := headPtr; curTag := headTag;
 U_link:    link[pushing] := cur;                          \* store_link(node, head.ptr)
 U_cas:     if (headPtr = cur /\ headTag = curTag) {
                headPtr := pushing; headTag := (curTag + 1) % TagMod;
                pushing := NULL;
            } else {
                cur := headPtr; curTag := headTag;
                goto U_link;
            };
        } or {
            \* ---------------- an owner overwrites a node's link word
            await Scribble /\ owned[self] # {};
            with (n \in owned[self], w \in Nodes \cup {NULL}) { link[n] := w; };
        } or {
            \* ---------------- steal_all: take the whole chain
            await UseSteal;
            stolen := StackSeq;
            headPtr := NULL;
            headTag := (headTag + 1) % TagMod;
 S_own:     owned[self] := owned[self] \cup {stolen[i] : i \in 1..Len(stolen)};
            stolen := <<>>;
        };
 L_next:
        skip;
    };
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, headPtr, headTag, link, owned

(* define statement *)
RECURSIVE Chain(_, _)
Chain(p, k) == IF p = NULL \/ k = 0 THEN <<>> ELSE <<p>> \o Chain(link[p], k - 1)
StackSeq == Chain(headPtr, Cardinality(Nodes) + 1)
OnStack == {StackSeq[i] : i \in 1..Len(StackSeq)}
AllOwned == UNION {owned[t] : t \in Threads}

NoCycle == Len(StackSeq) <= Cardinality(Nodes) /\ Cardinality(OnStack) = Len(StackSeq)

OwnedNotOnStack == AllOwned \cap OnStack = {}
OwnedOnce == \A t1, t2 \in Threads : t1 # t2 => owned[t1] \cap owned[t2] = {}

VARIABLES ops, cur, curTag, nxt, pushing, stolen

vars == << pc, headPtr, headTag, link, owned, ops, cur, curTag, nxt, pushing, 
           stolen >>

ProcSet == (Threads)

Init == (* Global variables *)
        /\ headPtr = (CHOOSE n \in Nodes : \A m \in Nodes : n <= m)
        /\ headTag = 0
        /\ link = [n \in Nodes |-> IF n + 1 \in Nodes THEN n + 1 ELSE NULL]
        /\ owned = [t \in Threads |-> {}]
        (* Process T *)
        /\ ops = [self \in Threads |-> 0]
        /\ cur = [self \in Threads |-> NULL]
        /\ curTag = [self \in Threads |-> 0]
        /\ nxt = [self \in Threads |-> NULL]
        /\ pushing = [self \in Threads |-> NULL]
        /\ stolen = [self \in Threads |-> <<>>]
        /\ pc = [self \in ProcSet |-> "L_loop"]

L_loop(self) == /\ pc[self] = "L_loop"
                /\ IF ops[self] < OpsPerThread
                      THEN /\ ops' = [ops EXCEPT ![self] = ops[self] + 1]
                           /\ \/ /\ pc' = [pc EXCEPT ![self] = "P_load"]
                                 /\ UNCHANGED <<headPtr, headTag, link, owned, pushing, stolen>>
                              \/ /\ owned[self] # {}
                                 /\ \E n \in owned[self]:
                                      /\ pushing' = [pushing EXCEPT ![self] = n]
                                      /\ owned' = [owned EXCEPT ![self] = owned[self] \ {n}]
                                 /\ pc' = [pc EXCEPT ![self] = "U_load"]
                                 /\ UNCHANGED <<headPtr, headTag, link, stolen>>
                              \/ /\ Scribble /\ owned[self] # {}
                                 /\ \E n \in owned[self]:
                                      \E w \in Nodes \cup {NULL}:
                                        link' = [link EXCEPT ![n] = w]
                                 /\ pc' = [pc EXCEPT ![self] = "L_next"]
                                 /\ UNCHANGED <<headPtr, headTag, owned, pushing, stolen>>
                              \/ /\ UseSteal
                                 /\ stolen' = [stolen EXCEPT ![self] = StackSeq]
                                 /\ headPtr' = NULL
                                 /\ headTag' = (headTag + 1) % TagMod
                                 /\ pc' = [pc EXCEPT ![self] = "S_own"]
                                 /\ UNCHANGED <<link, owned, pushing>>
                      ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                           /\ UNCHANGED << headPtr, headTag, link, owned, ops, 
                                           pushing, stolen >>
                /\ UNCHANGED << cur, curTag, nxt >>

L_next(self) == /\ pc[self] = "L_next"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "L_loop"]
                /\ UNCHANGED << headPtr, headTag, link, owned, ops, cur, 
                                curTag, nxt, pushing, stolen >>

P_load(self) == /\ pc[self] = "P_load"
                /\ cur' = [cur EXCEPT ![self] = headPtr]
                /\ curTag' = [curTag EXCEPT ![self] = headTag]
                /\ pc' = [pc EXCEPT ![self] = "P_check"]
                /\ UNCHANGED << headPtr, headTag, link, owned, ops, nxt, 
                                pushing, stolen >>

P_check(self) == /\ pc[self] = "P_check"
                 /\ IF cur[self] = NULL
                       THEN /\ pc' = [pc EXCEPT ![self] = "L_next"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "P_link"]
                 /\ UNCHANGED << headPtr, headTag, link, owned, ops, cur, 
                                 curTag, nxt, pushing, stolen >>

P_link(self) == /\ pc[self] = "P_link"
                /\ nxt' = [nxt EXCEPT ![self] = link[cur[self]]]
                /\ pc' = [pc EXCEPT ![self] = "P_cas"]
                /\ UNCHANGED << headPtr, headTag, link, owned, ops, cur, 
                                curTag, pushing, stolen >>

P_cas(self) == /\ pc[self] = "P_cas"
               /\ IF headPtr = cur[self] /\ headTag = curTag[self]
                     THEN /\ headPtr' = nxt[self]
                          /\ headTag' = (curTag[self] + 1) % TagMod
                          /\ Assert(cur[self] \notin AllOwned, 
                                    "Failure of assertion at line 70, column 17.")
                          /\ owned' = [owned EXCEPT ![self] = owned[self] \cup {cur[self]}]
                          /\ pc' = [pc EXCEPT ![self] = "P_clear"]
                          /\ UNCHANGED << cur, curTag >>
                     ELSE /\ cur' = [cur EXCEPT ![self] = headPtr]
                          /\ curTag' = [curTag EXCEPT ![self] = headTag]
                          /\ pc' = [pc EXCEPT ![self] = "P_check"]
                          /\ UNCHANGED << headPtr, headTag, owned >>
               /\ UNCHANGED << link, ops, nxt, pushing, stolen >>

P_clear(self) == /\ pc[self] = "P_clear"
                 /\ link' = [link EXCEPT ![cur[self]] = NULL]
                 /\ pc' = [pc EXCEPT ![self] = "L_next"]
                 /\ UNCHANGED << headPtr, headTag, owned, ops, cur, curTag, 
                                 nxt, pushing, stolen >>

U_load(self) == /\ pc[self] = "U_load"
                /\ cur' = [cur EXCEPT ![self] = headPtr]
                /\ curTag' = [curTag EXCEPT ![self] = headTag]
                /\ pc' = [pc EXCEPT ![self] = "U_link"]
                /\ UNCHANGED << headPtr, headTag, link, owned, ops, nxt, 
                                pushing, stolen >>

U_link(self) == /\ pc[self] = "U_link"
                /\ link' = [link EXCEPT ![pushing[self]] = cur[self]]
                /\ pc' = [pc EXCEPT ![self] = "U_cas"]
                /\ UNCHANGED << headPtr, headTag, owned, ops, cur, curTag, nxt, 
                                pushing, stolen >>

U_cas(self) == /\ pc[self] = "U_cas"
               /\ IF headPtr = cur[self] /\ headTag = curTag[self]
                     THEN /\ headPtr' = pushing[self]
                          /\ headTag' = (curTag[self] + 1) % TagMod
                          /\ pushing' = [pushing EXCEPT ![self] = NULL]
                          /\ pc' = [pc EXCEPT ![self] = "L_next"]
                          /\ UNCHANGED << cur, curTag >>
                     ELSE /\ cur' = [cur EXCEPT ![self] = headPtr]
                          /\ curTag' = [curTag EXCEPT ![self] = headTag]
                          /\ pc' = [pc EXCEPT ![self] = "U_link"]
                          /\ UNCHANGED << headPtr, headTag, pushing >>
               /\ UNCHANGED << link, owned, ops, nxt, stolen >>

S_own(self) == /\ pc[self] = "S_own"
               /\ owned' = [owned EXCEPT ![self] = owned[self] \cup {stolen[self][i] : i \in 1..Len(stolen[self])}]
               /\ stolen' = [stolen EXCEPT ![self] = <<>>]
               /\ pc' = [pc EXCEPT ![self] = "L_next"]
               /\ UNCHANGED << headPtr, headTag, link, ops, cur, curTag, nxt, 
                               pushing >>

T(self) == L_loop(self) \/ L_next(self) \/ P_load(self) \/ P_check(self)
              \/ P_link(self) \/ P_cas(self) \/ P_clear(self)
              \/ U_load(self) \/ U_link(self) \/ U_cas(self) \/ S_own(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Threads: T(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Threads : WF_vars(T(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* Every node is on the stack, owned, being pushed, or in a steal that has not yet been taken over.
NothingLost == \A n \in Nodes : \/ n \in OnStack \/ n \in AllOwned
                               \/ \E t \in Threads : n = pushing[t] \/ \E i \in 1..Len(stolen[t]) : stolen[t][i] = n

Safety == NoCycle /\ OwnedNotOnStack /\ OwnedOnce /\ NothingLost
=============================================================================
