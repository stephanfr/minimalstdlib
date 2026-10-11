----------------------- MODULE MpScGrowableRingQueue -----------------------
(***************************************************************************)
(* include/lockfree/mp_sc_growable_ring_queue: producers claim slots with  *)
(* a fetch_add on a (capacity, position) word; the single consumer drains  *)
(* each segment and, when a life is fully drained, frees its buffer and    *)
(* opens a new life with a new buffer (retire_and_grow), or closes a       *)
(* lightly loaded life early (maybe_shrink_active_segment).                *)
(*                                                                         *)
(* One label per shared access:                                            *)
(*   producer  try_claim_and_publish: fetch_add(enqueue_pos_) -> (cap, idx);*)
(*             read seg.buffer_; construct the element; ready_.store       *)
(*   consumer  try_pop_from: read ready_; take the element; ready_ = false; *)
(*             ++dequeue_state_; at capacity_: retire_and_grow = free the  *)
(*             old buffer, install the new buffer and capacity_,           *)
(*             enqueue_pos_.store(pack(new_cap, 0))                        *)
(*             maybe_shrink_active_segment: CAS (cap, pos) -> (pos, pos)   *)
(*                                                                         *)
(* Abstractions: element values are producer-tagged integers; the position *)
(* clamp (2^30 failed claims) is not reachable at these sizes; allocation  *)
(* never fails (the recycle-in-place path reuses the same buffer, which    *)
(* is never freed, so it is strictly safer).                               *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    Producers,      \* producer ids, e.g. {1, 2}
    PushesPer,      \* push_back calls per producer
    InitCap,        \* initial segment capacity
    MaxCap,         \* growth ceiling
    MaxBuffers,     \* bound on buffer allocations over the run
    UseShrink       \* consumer may close lightly loaded lives early

Segs == {1, 2}
Other(s) == 3 - s
CONSUMER == 0
Val(p, k) == 100 * p + k                    \* the k-th element pushed by producer p

(* --algorithm MpScGrowableRingQueue {
variables
    pos = [s \in Segs |-> 0],               \* enqueue_pos_: position half
    cap = [s \in Segs |-> InitCap],         \* enqueue_pos_: capacity half
    buf = [s \in Segs |-> s],               \* seg.buffer_ (buffer ids 1, 2 to start)
    bufCap = [b \in 1..MaxBuffers |-> IF b \in Segs THEN InitCap ELSE 0],
    nextBuf = 3,
    freed = {},
    slotVal = [b \in 1..MaxBuffers |-> [i \in 0..(MaxCap - 1) |-> 0]],
    ready = [b \in 1..MaxBuffers |-> [i \in 0..(MaxCap - 1) |-> FALSE]],
    ccap = [s \in Segs |-> InitCap],        \* seg.capacity_ (consumer-owned)
    dq = [s \in Segs |-> 0],                \* seg.dequeue_state_
    producerTarget = 1,
    pushed = {},                            \* values whose push_back returned true
    consumed = <<>>;                        \* values popped, in order

fair process (P \in Producers)
variables k = 0, att = 0, tgt = 1, idx = 0, c = 0, bp = 0;
{
 P_loop:
    while (k < PushesPer) {
        k := k + 1;
        tgt := producerTarget; att := 0;
 P_try:
        while (att < 2) {
            att := att + 1;
 P_claim:                                                 \* enqueue_pos_.fetch_add(1)
            idx := pos[tgt]; c := cap[tgt];
            pos[tgt] := pos[tgt] + 1;
            if (idx >= c) {                               \* full: try the next segment
                tgt := Other(tgt);
                producerTarget := tgt;
            } else {
 P_buf:         bp := buf[tgt];                           \* seg.buffer_ (plain read)
 P_write:       assert bp \notin freed /\ idx < bufCap[bp];
                slotVal[bp][idx] := Val(self, k);         \* construct the element
 P_pub:         assert bp \notin freed;
                ready[bp][idx] := TRUE;                   \* ready_.store(true, release)
                pushed := pushed \cup {Val(self, k)};
                goto P_next;
            };
        };
 P_next:
        skip;
    };
}

fair process (C = CONSUMER)
variables cs = 1, cb = 0, nc = 0, nb = 0;
{
 C_loop:
    while (TRUE) {
        either {
            \* ---------------- pop from segment cs (pop_front tries both segments)
            with (seg \in Segs) { cs := seg; };
            await dq[cs] < ccap[cs] /\ ready[buf[cs]][dq[cs]];
            cb := buf[cs];
            consumed := Append(consumed, slotVal[cb][dq[cs]]);
            ready[cb][dq[cs]] := FALSE;
            slotVal[cb][dq[cs]] := 0;
            dq[cs] := dq[cs] + 1;
            if (dq[cs] = ccap[cs]) {
 C_grow:        \* retire_and_grow: allocate, free the old buffer, install, open the new life
                cb := buf[cs];
                if (nextBuf <= MaxBuffers) {
                    nb := nextBuf; nextBuf := nextBuf + 1;
                    nc := IF ccap[cs] < bufCap[cb] THEN (IF ccap[cs] > 1 THEN ccap[cs] - 1 ELSE 1)
                          ELSE (IF ccap[Other(cs)] + 1 <= MaxCap THEN ccap[Other(cs)] + 1 ELSE MaxCap);
                    bufCap[nb] := nc;
                } else {
                    nb := cb; nc := bufCap[cb];          \* allocation failed (or bound reached): recycle in place
                    goto C_install;
                };
 C_free:        freed := freed \cup {cb};
 C_install:     buf[cs] := nb; ccap[cs] := nc; dq[cs] := 0;
 C_open:        pos[cs] := 0; cap[cs] := nc;                \* enqueue_pos_.store(pack(nc, 0), release)
            };
        } or {
            \* ---------------- maybe_shrink_active_segment on the producer target
            await UseShrink;
            cs := producerTarget;
            await pos[cs] > 0 /\ pos[cs] < cap[cs] /\ pos[cs] * 2 < cap[cs];
            cap[cs] := pos[cs];                             \* CAS (cap, pos) -> (pos, pos)
 C_capped:  ccap[cs] := pos[cs];
            if (dq[cs] >= ccap[cs]) { goto C_grow; };
        } or {
            \* ---------------- done: every producer finished and everything pushed was popped
            await \A p \in Producers : pc[p] = "Done";
            await Len(consumed) = Cardinality(pushed);
            goto C_done;
        };
    };
 C_done:
    skip;
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, pos, cap, buf, bufCap, nextBuf, freed, slotVal, ready, ccap, dq, 
          producerTarget, pushed, consumed, k, att, tgt, idx, c, bp, cs, cb, 
          nc, nb

vars == << pc, pos, cap, buf, bufCap, nextBuf, freed, slotVal, ready, ccap, 
           dq, producerTarget, pushed, consumed, k, att, tgt, idx, c, bp, cs, 
           cb, nc, nb >>

ProcSet == (Producers) \cup {CONSUMER}

Init == (* Global variables *)
        /\ pos = [s \in Segs |-> 0]
        /\ cap = [s \in Segs |-> InitCap]
        /\ buf = [s \in Segs |-> s]
        /\ bufCap = [b \in 1..MaxBuffers |-> IF b \in Segs THEN InitCap ELSE 0]
        /\ nextBuf = 3
        /\ freed = {}
        /\ slotVal = [b \in 1..MaxBuffers |-> [i \in 0..(MaxCap - 1) |-> 0]]
        /\ ready = [b \in 1..MaxBuffers |-> [i \in 0..(MaxCap - 1) |-> FALSE]]
        /\ ccap = [s \in Segs |-> InitCap]
        /\ dq = [s \in Segs |-> 0]
        /\ producerTarget = 1
        /\ pushed = {}
        /\ consumed = <<>>
        (* Process P *)
        /\ k = [self \in Producers |-> 0]
        /\ att = [self \in Producers |-> 0]
        /\ tgt = [self \in Producers |-> 1]
        /\ idx = [self \in Producers |-> 0]
        /\ c = [self \in Producers |-> 0]
        /\ bp = [self \in Producers |-> 0]
        (* Process C *)
        /\ cs = 1
        /\ cb = 0
        /\ nc = 0
        /\ nb = 0
        /\ pc = [self \in ProcSet |-> CASE self \in Producers -> "P_loop"
                                        [] self = CONSUMER -> "C_loop"]

P_loop(self) == /\ pc[self] = "P_loop"
                /\ IF k[self] < PushesPer
                      THEN /\ k' = [k EXCEPT ![self] = k[self] + 1]
                           /\ tgt' = [tgt EXCEPT ![self] = producerTarget]
                           /\ att' = [att EXCEPT ![self] = 0]
                           /\ pc' = [pc EXCEPT ![self] = "P_try"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                           /\ UNCHANGED << k, att, tgt >>
                /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, freed, slotVal, 
                                ready, ccap, dq, producerTarget, pushed, 
                                consumed, idx, c, bp, cs, cb, nc, nb >>

P_try(self) == /\ pc[self] = "P_try"
               /\ IF att[self] < 2
                     THEN /\ att' = [att EXCEPT ![self] = att[self] + 1]
                          /\ pc' = [pc EXCEPT ![self] = "P_claim"]
                     ELSE /\ pc' = [pc EXCEPT ![self] = "P_next"]
                          /\ att' = att
               /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, freed, slotVal, 
                               ready, ccap, dq, producerTarget, pushed, 
                               consumed, k, tgt, idx, c, bp, cs, cb, nc, nb >>

P_claim(self) == /\ pc[self] = "P_claim"
                 /\ idx' = [idx EXCEPT ![self] = pos[tgt[self]]]
                 /\ c' = [c EXCEPT ![self] = cap[tgt[self]]]
                 /\ pos' = [pos EXCEPT ![tgt[self]] = pos[tgt[self]] + 1]
                 /\ IF idx'[self] >= c'[self]
                       THEN /\ tgt' = [tgt EXCEPT ![self] = Other(tgt[self])]
                            /\ producerTarget' = tgt'[self]
                            /\ pc' = [pc EXCEPT ![self] = "P_try"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "P_buf"]
                            /\ UNCHANGED << producerTarget, tgt >>
                 /\ UNCHANGED << cap, buf, bufCap, nextBuf, freed, slotVal, 
                                 ready, ccap, dq, pushed, consumed, k, att, bp, 
                                 cs, cb, nc, nb >>

P_buf(self) == /\ pc[self] = "P_buf"
               /\ bp' = [bp EXCEPT ![self] = buf[tgt[self]]]
               /\ pc' = [pc EXCEPT ![self] = "P_write"]
               /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, freed, slotVal, 
                               ready, ccap, dq, producerTarget, pushed, 
                               consumed, k, att, tgt, idx, c, cs, cb, nc, nb >>

P_write(self) == /\ pc[self] = "P_write"
                 /\ Assert(bp[self] \notin freed /\ idx[self] < bufCap[bp[self]], 
                           "Failure of assertion at line 72, column 17.")
                 /\ slotVal' = [slotVal EXCEPT ![bp[self]][idx[self]] = Val(self, k[self])]
                 /\ pc' = [pc EXCEPT ![self] = "P_pub"]
                 /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, freed, ready, 
                                 ccap, dq, producerTarget, pushed, consumed, k, 
                                 att, tgt, idx, c, bp, cs, cb, nc, nb >>

P_pub(self) == /\ pc[self] = "P_pub"
               /\ Assert(bp[self] \notin freed, 
                         "Failure of assertion at line 74, column 17.")
               /\ ready' = [ready EXCEPT ![bp[self]][idx[self]] = TRUE]
               /\ pushed' = (pushed \cup {Val(self, k[self])})
               /\ pc' = [pc EXCEPT ![self] = "P_next"]
               /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, freed, slotVal, 
                               ccap, dq, producerTarget, consumed, k, att, tgt, 
                               idx, c, bp, cs, cb, nc, nb >>

P_next(self) == /\ pc[self] = "P_next"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "P_loop"]
                /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, freed, slotVal, 
                                ready, ccap, dq, producerTarget, pushed, 
                                consumed, k, att, tgt, idx, c, bp, cs, cb, nc, 
                                nb >>

P(self) == P_loop(self) \/ P_try(self) \/ P_claim(self) \/ P_buf(self)
              \/ P_write(self) \/ P_pub(self) \/ P_next(self)

C_loop == /\ pc[CONSUMER] = "C_loop"
          /\ \/ /\ \E seg \in Segs:
                     cs' = seg
                /\ dq[cs'] < ccap[cs'] /\ ready[buf[cs']][dq[cs']]
                /\ cb' = buf[cs']
                /\ consumed' = Append(consumed, slotVal[cb'][dq[cs']])
                /\ ready' = [ready EXCEPT ![cb'][dq[cs']] = FALSE]
                /\ slotVal' = [slotVal EXCEPT ![cb'][dq[cs']] = 0]
                /\ dq' = [dq EXCEPT ![cs'] = dq[cs'] + 1]
                /\ IF dq'[cs'] = ccap[cs']
                      THEN /\ pc' = [pc EXCEPT ![CONSUMER] = "C_grow"]
                      ELSE /\ pc' = [pc EXCEPT ![CONSUMER] = "C_loop"]
                /\ cap' = cap
             \/ /\ UseShrink
                /\ cs' = producerTarget
                /\ pos[cs'] > 0 /\ pos[cs'] < cap[cs'] /\ pos[cs'] * 2 < cap[cs']
                /\ cap' = [cap EXCEPT ![cs'] = pos[cs']]
                /\ pc' = [pc EXCEPT ![CONSUMER] = "C_capped"]
                /\ UNCHANGED <<slotVal, ready, dq, consumed, cb>>
             \/ /\ \A p \in Producers : pc[p] = "Done"
                /\ Len(consumed) = Cardinality(pushed)
                /\ pc' = [pc EXCEPT ![CONSUMER] = "C_done"]
                /\ UNCHANGED <<cap, slotVal, ready, dq, consumed, cs, cb>>
          /\ UNCHANGED << pos, buf, bufCap, nextBuf, freed, ccap, 
                          producerTarget, pushed, k, att, tgt, idx, c, bp, nc, 
                          nb >>

C_grow == /\ pc[CONSUMER] = "C_grow"
          /\ cb' = buf[cs]
          /\ IF nextBuf <= MaxBuffers
                THEN /\ nb' = nextBuf
                     /\ nextBuf' = nextBuf + 1
                     /\ nc' = (IF ccap[cs] < bufCap[cb'] THEN (IF ccap[cs] > 1 THEN ccap[cs] - 1 ELSE 1)
                               ELSE (IF ccap[Other(cs)] + 1 <= MaxCap THEN ccap[Other(cs)] + 1 ELSE MaxCap))
                     /\ bufCap' = [bufCap EXCEPT ![nb'] = nc']
                     /\ pc' = [pc EXCEPT ![CONSUMER] = "C_free"]
                ELSE /\ nb' = cb'
                     /\ nc' = bufCap[cb']
                     /\ pc' = [pc EXCEPT ![CONSUMER] = "C_install"]
                     /\ UNCHANGED << bufCap, nextBuf >>
          /\ UNCHANGED << pos, cap, buf, freed, slotVal, ready, ccap, dq, 
                          producerTarget, pushed, consumed, k, att, tgt, idx, 
                          c, bp, cs >>

C_free == /\ pc[CONSUMER] = "C_free"
          /\ freed' = (freed \cup {cb})
          /\ pc' = [pc EXCEPT ![CONSUMER] = "C_install"]
          /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, slotVal, ready, ccap, 
                          dq, producerTarget, pushed, consumed, k, att, tgt, 
                          idx, c, bp, cs, cb, nc, nb >>

C_install == /\ pc[CONSUMER] = "C_install"
             /\ buf' = [buf EXCEPT ![cs] = nb]
             /\ ccap' = [ccap EXCEPT ![cs] = nc]
             /\ dq' = [dq EXCEPT ![cs] = 0]
             /\ pc' = [pc EXCEPT ![CONSUMER] = "C_open"]
             /\ UNCHANGED << pos, cap, bufCap, nextBuf, freed, slotVal, ready, 
                             producerTarget, pushed, consumed, k, att, tgt, 
                             idx, c, bp, cs, cb, nc, nb >>

C_open == /\ pc[CONSUMER] = "C_open"
          /\ pos' = [pos EXCEPT ![cs] = 0]
          /\ cap' = [cap EXCEPT ![cs] = nc]
          /\ pc' = [pc EXCEPT ![CONSUMER] = "C_loop"]
          /\ UNCHANGED << buf, bufCap, nextBuf, freed, slotVal, ready, ccap, 
                          dq, producerTarget, pushed, consumed, k, att, tgt, 
                          idx, c, bp, cs, cb, nc, nb >>

C_capped == /\ pc[CONSUMER] = "C_capped"
            /\ ccap' = [ccap EXCEPT ![cs] = pos[cs]]
            /\ IF dq[cs] >= ccap'[cs]
                  THEN /\ pc' = [pc EXCEPT ![CONSUMER] = "C_grow"]
                  ELSE /\ pc' = [pc EXCEPT ![CONSUMER] = "C_loop"]
            /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, freed, slotVal, 
                            ready, dq, producerTarget, pushed, consumed, k, 
                            att, tgt, idx, c, bp, cs, cb, nc, nb >>

C_done == /\ pc[CONSUMER] = "C_done"
          /\ TRUE
          /\ pc' = [pc EXCEPT ![CONSUMER] = "Done"]
          /\ UNCHANGED << pos, cap, buf, bufCap, nextBuf, freed, slotVal, 
                          ready, ccap, dq, producerTarget, pushed, consumed, k, 
                          att, tgt, idx, c, bp, cs, cb, nc, nb >>

C == C_loop \/ C_grow \/ C_free \/ C_install \/ C_open \/ C_capped
        \/ C_done

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == C
           \/ (\E self \in Producers: P(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Producers : WF_vars(P(self))
        /\ WF_vars(C)

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

ConsumedSet == {consumed[i] : i \in 1..Len(consumed)}
\* Nothing popped that was not pushed, nothing popped twice (0 would be an empty slot)
NoGhostOrDuplicate == /\ \A i \in 1..Len(consumed) : consumed[i] # 0
                      /\ Cardinality(ConsumedSet) = Len(consumed)
\* At the end, every successful push was popped
NothingLostAtEnd == (\A p \in ProcSet : pc[p] = "Done") => ConsumedSet = pushed
Safety == NoGhostOrDuplicate /\ NothingLostAtEnd
=============================================================================
