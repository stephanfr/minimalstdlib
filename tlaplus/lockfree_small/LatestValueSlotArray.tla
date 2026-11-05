------------------------ MODULE LatestValueSlotArray ------------------------
(***************************************************************************)
(* include/lockfree/latest_value_slot_array: a seqlock over NumSlots      *)
(* slots, a published word current_ = (seq << 32 | STORED_FLAG | idx),    *)
(* and a writer_busy_ flag.                                               *)
(*   store: exchange(writer_busy_, 1) -- a 1 means another store is in    *)
(*          flight, so drop and return FAILURE.  next = idx + 1;          *)
(*          seq_[next] := odd; write each payload word; seq_[next] :=     *)
(*          even; current_ := (even, next); writer_busy_ := 0.            *)
(*   load:  (exp, idx) = current_; s1 = seq_[idx]; copy each word;        *)
(*          s2 = seq_[idx]; done if s1 = s2 = exp, else retry.  The load  *)
(*          returns EMPTY iff STORED_FLAG was clear in current_.          *)
(*          With CheckPublishedSeq = FALSE the reader instead accepts any *)
(*          even s1 = s2 -- the first version of the C++, which can       *)
(*          return a value the writer has not published yet.              *)
(* Each stored value is a fresh generation number written into every      *)
(* word, so a torn snapshot shows up as two different words.  Stores are  *)
(* serialised by writer_busy_ and linearised at their write of current_,  *)
(* so published generations are 0, 1, 2, ...; a load is linearisable iff  *)
(* it returns one that was published at some instant while it ran, i.e.   *)
(* one in lo..published where lo is `published` when it started, and it   *)
(* returns EMPTY iff it returns generation 0, the initial zero value.     *)
(* Seq counters wrap at SeqMod (2^32 in C++).  IrqPairs holds             *)
(* <<handler, host>>: while the handler is inside an operation its host   *)
(* (the thread it interrupted, on the same CPU) cannot run.  Processes    *)
(* outside FairProcs may stop forever at any step, e.g. a preempted       *)
(* writer holding writer_busy_.  Dead locals are reset to keep the state  *)
(* space small.                                                           *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS NumSlots, WordCount, Writers, Readers, StoresPer, ReadsPer,
          SeqMod, CheckPublishedSeq, FairProcs, IrqPairs

Slots == 0..(NumSlots - 1)
Words == 0..(WordCount - 1)

\* IrqPairs values for the .cfg files, which cannot spell tuples.
IrqNested == {<<3, 1>>, <<2, 3>>}      \* reader 3 interrupts writer 1; writer 2 interrupts reader 3

(* --algorithm LatestValueSlotArray {
variables
    seq = [s \in Slots |-> 0],
    words = [s \in Slots |-> [w \in Words |-> 0]],
    curIdx = 0, curSeq = 0,                 \* the two halves of current_, written as one word
    curStored = FALSE,                      \*   with STORED_FLAG, set by every publish
    busy = 0,
    published = 0;                          \* ghost: latest published generation

process (Wr \in Writers)
variables k = 0, nxt = 0, sq = 0, val = 0, wi = 0;
{
 W:      while (k < StoresPer) {
             k := k + 1;
 W_xchg:     if (busy = 1) { goto W; }      \* dropped: another store in flight
             else { busy := 1; };
 W_next:     nxt := (curIdx + 1) % NumSlots;    \* current_ and seq_ are written only
             sq := seq[(curIdx + 1) % NumSlots]; \*   by the writer_busy_ holder
             val := published + 1;
 W_odd:      seq[nxt] := (sq + 1) % SeqMod;
 W_word:     while (wi < WordCount) {
                 words[nxt][wi] := val;
                 wi := wi + 1;
             };
 W_even:     seq[nxt] := (sq + 2) % SeqMod;
 W_pub:      curIdx := nxt;
             curSeq := (sq + 2) % SeqMod;
             curStored := TRUE;
             published := val;
 W_rel:      busy := 0;
             nxt := 0; sq := 0; val := 0; wi := 0;
         };
}

process (Rd \in Readers)
variables j = 0, lo = 0, idx = 0, exp = 0, stored = FALSE, s1 = 0, s2 = 0, ri = 0,
          buf = [w \in Words |-> 0];
{
 R:      while (j < ReadsPer) {
             j := j + 1;
             lo := published;
 R_cur:      idx := curIdx;
             exp := curSeq;
             stored := curStored;
             ri := 0;
 R_s1:       s1 := seq[idx];
             if (IF CheckPublishedSeq THEN s1 # exp ELSE s1 % 2 = 1) { goto R_cur; };
 R_word:     while (ri < WordCount) {
                 buf[ri] := words[idx][ri];
                 ri := ri + 1;
             };
 R_s2:       s2 := seq[idx];
             if (s1 # s2) { goto R_cur; };
 R_ret:      assert \A w \in Words : buf[w] = buf[0];         \* no torn snapshot
             assert buf[0] >= lo /\ buf[0] <= published;      \* linearisable
             assert stored = (buf[0] # 0);                     \* EMPTY iff the initial value
             lo := 0; idx := 0; exp := 0; stored := FALSE; s1 := 0; s2 := 0; ri := 0;
             buf := [w \in Words |-> 0];
         };
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, seq, words, curIdx, curSeq, curStored, busy, published, k, nxt, 
          sq, val, wi, j, lo, idx, exp, stored, s1, s2, ri, buf

vars == << pc, seq, words, curIdx, curSeq, curStored, busy, published, k, nxt, 
           sq, val, wi, j, lo, idx, exp, stored, s1, s2, ri, buf >>

ProcSet == (Writers) \cup (Readers)

Init == (* Global variables *)
        /\ seq = [s \in Slots |-> 0]
        /\ words = [s \in Slots |-> [w \in Words |-> 0]]
        /\ curIdx = 0
        /\ curSeq = 0
        /\ curStored = FALSE
        /\ busy = 0
        /\ published = 0
        (* Process Wr *)
        /\ k = [self \in Writers |-> 0]
        /\ nxt = [self \in Writers |-> 0]
        /\ sq = [self \in Writers |-> 0]
        /\ val = [self \in Writers |-> 0]
        /\ wi = [self \in Writers |-> 0]
        (* Process Rd *)
        /\ j = [self \in Readers |-> 0]
        /\ lo = [self \in Readers |-> 0]
        /\ idx = [self \in Readers |-> 0]
        /\ exp = [self \in Readers |-> 0]
        /\ stored = [self \in Readers |-> FALSE]
        /\ s1 = [self \in Readers |-> 0]
        /\ s2 = [self \in Readers |-> 0]
        /\ ri = [self \in Readers |-> 0]
        /\ buf = [self \in Readers |-> [w \in Words |-> 0]]
        /\ pc = [self \in ProcSet |-> CASE self \in Writers -> "W"
                                        [] self \in Readers -> "R"]

W(self) == /\ pc[self] = "W"
           /\ IF k[self] < StoresPer
                 THEN /\ k' = [k EXCEPT ![self] = k[self] + 1]
                      /\ pc' = [pc EXCEPT ![self] = "W_xchg"]
                 ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                      /\ k' = k
           /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, busy, 
                           published, nxt, sq, val, wi, j, lo, idx, exp, 
                           stored, s1, s2, ri, buf >>

W_xchg(self) == /\ pc[self] = "W_xchg"
                /\ IF busy = 1
                      THEN /\ pc' = [pc EXCEPT ![self] = "W"]
                           /\ busy' = busy
                      ELSE /\ busy' = 1
                           /\ pc' = [pc EXCEPT ![self] = "W_next"]
                /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, 
                                published, k, nxt, sq, val, wi, j, lo, idx, 
                                exp, stored, s1, s2, ri, buf >>

W_next(self) == /\ pc[self] = "W_next"
                /\ nxt' = [nxt EXCEPT ![self] = (curIdx + 1) % NumSlots]
                /\ sq' = [sq EXCEPT ![self] = seq[(curIdx + 1) % NumSlots]]
                /\ val' = [val EXCEPT ![self] = published + 1]
                /\ pc' = [pc EXCEPT ![self] = "W_odd"]
                /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, busy, 
                                published, k, wi, j, lo, idx, exp, stored, s1, 
                                s2, ri, buf >>

W_odd(self) == /\ pc[self] = "W_odd"
               /\ seq' = [seq EXCEPT ![nxt[self]] = (sq[self] + 1) % SeqMod]
               /\ pc' = [pc EXCEPT ![self] = "W_word"]
               /\ UNCHANGED << words, curIdx, curSeq, curStored, busy, 
                               published, k, nxt, sq, val, wi, j, lo, idx, exp, 
                               stored, s1, s2, ri, buf >>

W_word(self) == /\ pc[self] = "W_word"
                /\ IF wi[self] < WordCount
                      THEN /\ words' = [words EXCEPT ![nxt[self]][wi[self]] = val[self]]
                           /\ wi' = [wi EXCEPT ![self] = wi[self] + 1]
                           /\ pc' = [pc EXCEPT ![self] = "W_word"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "W_even"]
                           /\ UNCHANGED << words, wi >>
                /\ UNCHANGED << seq, curIdx, curSeq, curStored, busy, 
                                published, k, nxt, sq, val, j, lo, idx, exp, 
                                stored, s1, s2, ri, buf >>

W_even(self) == /\ pc[self] = "W_even"
                /\ seq' = [seq EXCEPT ![nxt[self]] = (sq[self] + 2) % SeqMod]
                /\ pc' = [pc EXCEPT ![self] = "W_pub"]
                /\ UNCHANGED << words, curIdx, curSeq, curStored, busy, 
                                published, k, nxt, sq, val, wi, j, lo, idx, 
                                exp, stored, s1, s2, ri, buf >>

W_pub(self) == /\ pc[self] = "W_pub"
               /\ curIdx' = nxt[self]
               /\ curSeq' = (sq[self] + 2) % SeqMod
               /\ curStored' = TRUE
               /\ published' = val[self]
               /\ pc' = [pc EXCEPT ![self] = "W_rel"]
               /\ UNCHANGED << seq, words, busy, k, nxt, sq, val, wi, j, lo, 
                               idx, exp, stored, s1, s2, ri, buf >>

W_rel(self) == /\ pc[self] = "W_rel"
               /\ busy' = 0
               /\ nxt' = [nxt EXCEPT ![self] = 0]
               /\ sq' = [sq EXCEPT ![self] = 0]
               /\ val' = [val EXCEPT ![self] = 0]
               /\ wi' = [wi EXCEPT ![self] = 0]
               /\ pc' = [pc EXCEPT ![self] = "W"]
               /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, 
                               published, k, j, lo, idx, exp, stored, s1, s2, 
                               ri, buf >>

Wr(self) == W(self) \/ W_xchg(self) \/ W_next(self) \/ W_odd(self)
               \/ W_word(self) \/ W_even(self) \/ W_pub(self)
               \/ W_rel(self)

R(self) == /\ pc[self] = "R"
           /\ IF j[self] < ReadsPer
                 THEN /\ j' = [j EXCEPT ![self] = j[self] + 1]
                      /\ lo' = [lo EXCEPT ![self] = published]
                      /\ pc' = [pc EXCEPT ![self] = "R_cur"]
                 ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                      /\ UNCHANGED << j, lo >>
           /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, busy, 
                           published, k, nxt, sq, val, wi, idx, exp, stored, 
                           s1, s2, ri, buf >>

R_cur(self) == /\ pc[self] = "R_cur"
               /\ idx' = [idx EXCEPT ![self] = curIdx]
               /\ exp' = [exp EXCEPT ![self] = curSeq]
               /\ stored' = [stored EXCEPT ![self] = curStored]
               /\ ri' = [ri EXCEPT ![self] = 0]
               /\ pc' = [pc EXCEPT ![self] = "R_s1"]
               /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, busy, 
                               published, k, nxt, sq, val, wi, j, lo, s1, s2, 
                               buf >>

R_s1(self) == /\ pc[self] = "R_s1"
              /\ s1' = [s1 EXCEPT ![self] = seq[idx[self]]]
              /\ IF IF CheckPublishedSeq THEN s1'[self] # exp[self] ELSE s1'[self] % 2 = 1
                    THEN /\ pc' = [pc EXCEPT ![self] = "R_cur"]
                    ELSE /\ pc' = [pc EXCEPT ![self] = "R_word"]
              /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, busy, 
                              published, k, nxt, sq, val, wi, j, lo, idx, exp, 
                              stored, s2, ri, buf >>

R_word(self) == /\ pc[self] = "R_word"
                /\ IF ri[self] < WordCount
                      THEN /\ buf' = [buf EXCEPT ![self][ri[self]] = words[idx[self]][ri[self]]]
                           /\ ri' = [ri EXCEPT ![self] = ri[self] + 1]
                           /\ pc' = [pc EXCEPT ![self] = "R_word"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "R_s2"]
                           /\ UNCHANGED << ri, buf >>
                /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, busy, 
                                published, k, nxt, sq, val, wi, j, lo, idx, 
                                exp, stored, s1, s2 >>

R_s2(self) == /\ pc[self] = "R_s2"
              /\ s2' = [s2 EXCEPT ![self] = seq[idx[self]]]
              /\ IF s1[self] # s2'[self]
                    THEN /\ pc' = [pc EXCEPT ![self] = "R_cur"]
                    ELSE /\ pc' = [pc EXCEPT ![self] = "R_ret"]
              /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, busy, 
                              published, k, nxt, sq, val, wi, j, lo, idx, exp, 
                              stored, s1, ri, buf >>

R_ret(self) == /\ pc[self] = "R_ret"
               /\ Assert(\A w \in Words : buf[self][w] = buf[self][0], 
                         "Failure of assertion at line 94, column 14.")
               /\ Assert(buf[self][0] >= lo[self] /\ buf[self][0] <= published, 
                         "Failure of assertion at line 95, column 14.")
               /\ Assert(stored[self] = (buf[self][0] # 0), 
                         "Failure of assertion at line 96, column 14.")
               /\ lo' = [lo EXCEPT ![self] = 0]
               /\ idx' = [idx EXCEPT ![self] = 0]
               /\ exp' = [exp EXCEPT ![self] = 0]
               /\ stored' = [stored EXCEPT ![self] = FALSE]
               /\ s1' = [s1 EXCEPT ![self] = 0]
               /\ s2' = [s2 EXCEPT ![self] = 0]
               /\ ri' = [ri EXCEPT ![self] = 0]
               /\ buf' = [buf EXCEPT ![self] = [w \in Words |-> 0]]
               /\ pc' = [pc EXCEPT ![self] = "R"]
               /\ UNCHANGED << seq, words, curIdx, curSeq, curStored, busy, 
                               published, k, nxt, sq, val, wi, j >>

Rd(self) == R(self) \/ R_cur(self) \/ R_s1(self) \/ R_word(self)
               \/ R_s2(self) \/ R_ret(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Writers: Wr(self))
           \/ (\E self \in Readers: Rd(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* -------------------------------------------------------------------------
\* Interrupts and fairness.  A host is frozen while a handler that
\* interrupted it is between the first and last steps of an operation.
\* -------------------------------------------------------------------------

Frozen(p) == \E pr \in IrqPairs : pr[2] = p /\ pc[pr[1]] \notin {"W", "R", "Done"}

Step(p) == (p \in Writers /\ Wr(p)) \/ (p \in Readers /\ Rd(p))

LiveNext == (\E p \in ProcSet : ~Frozen(p) /\ Step(p)) \/ Terminating

LiveSpec == /\ Init /\ [][LiveNext]_vars
            /\ \A p \in FairProcs : WF_vars(~Frozen(p) /\ Step(p))

\* Every fair process finishes all its operations, whatever the unfair ones do.
FairDone == <>(\A p \in FairProcs : pc[p] = "Done")

\* -------------------------------------------------------------------------
\* Invariants
\* -------------------------------------------------------------------------

InStore == {"W_next", "W_odd", "W_word", "W_even", "W_pub", "W_rel"}

\* writer_busy_ is 1 exactly while one store is in flight.
BusyOwned == /\ (busy = 1) <=> (\E w \in Writers : pc[w] \in InStore)
             /\ \A w1, w2 \in Writers : (pc[w1] \in InStore /\ pc[w2] \in InStore) => w1 = w2

\* The in-flight store never writes the published slot, so a reader of the
\* published slot never waits on it.
NeverWritesCurrent == \A w \in Writers : pc[w] \in {"W_odd", "W_word", "W_even"} => nxt[w] # curIdx

\* current_ names a stable slot, and its seq half matches that slot.
CurrentIsStable == seq[curIdx] = curSeq /\ curSeq % 2 = 0

Safety == BusyOwned /\ NeverWritesCurrent /\ CurrentIsStable
=============================================================================
