----------------------------- MODULE SpscQueue -----------------------------
(***************************************************************************)
(* include/lockfree/spsc_queue: ring of Size slots, one slot always empty. *)
(*   push_back: current = write_index (own); next = current + 1 mod Size;  *)
(*              if next != read_index.load: construct elements[current];  *)
(*              write_index.store(next)                                   *)
(*   pop_front: current = read_index (own); if current == write_index.load *)
(*              empty; else take elements[current]; destroy;              *)
(*              read_index.store(next)                                    *)
(*   popFront:  as pop_front, discarding (returns if empty)                *)
(*   ~spsc_queue: destroys elements[read .. write)                        *)
(* Checked: neither side touches a slot the other owns (the element being *)
(* written / read), FIFO, nothing lost, destructor destroys exactly the   *)
(* live elements.                                                          *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Size, Pushes, Pops

(* --algorithm SpscQueue {
variables
    w = 0, r = 0,
    slot = [i \in 0..(Size - 1) |-> 0],
    writing = -1, reading = -1,            \* slot each side is accessing
    pushedSeq = <<>>, poppedSeq = <<>>, destroyed = <<>>;

fair process (Prod = 0)
variables n = 0, cur = 0, nxt = 0;
{
 U: while (n < Pushes) {
        n := n + 1;
 U1:    cur := w; nxt := (w + 1) % Size;
 U2:    if (nxt # r) {                                    \* read_index.load(acquire)
            writing := cur;
 U3:        assert reading # cur;
            slot[cur] := n;                               \* construct
 U4:        writing := -1; w := nxt;                      \* write_index.store(release)
            pushedSeq := Append(pushedSeq, n);
        };
    };
}

fair process (Cons = 1)
variables m = 0, cur = 0;
{
 O: while (m < Pops) {
        m := m + 1;
 O1:    cur := r;
 O2:    if (cur # w) {                                    \* write_index.load(acquire)
            reading := cur;
 O3:        assert writing # cur;
            poppedSeq := Append(poppedSeq, slot[cur]);    \* move out + destroy
            slot[cur] := 0;
 O4:        reading := -1; r := (cur + 1) % Size;         \* read_index.store(release)
        };
    };
 D: \* ~spsc_queue once the producer is done (precondition: no concurrent use)
    await pc[0] = "Done";
 D_loop:
    while (r # w) {
        destroyed := Append(destroyed, slot[r]);
        slot[r] := 0;
        r := (r + 1) % Size;
    };
}
} *)

\* BEGIN TRANSLATION
\* Process variable cur of process Prod at line 28 col 18 changed to cur_
VARIABLES pc, w, r, slot, writing, reading, pushedSeq, poppedSeq, destroyed, 
          n, cur_, nxt, m, cur

vars == << pc, w, r, slot, writing, reading, pushedSeq, poppedSeq, destroyed, 
           n, cur_, nxt, m, cur >>

ProcSet == {0} \cup {1}

Init == (* Global variables *)
        /\ w = 0
        /\ r = 0
        /\ slot = [i \in 0..(Size - 1) |-> 0]
        /\ writing = -1
        /\ reading = -1
        /\ pushedSeq = <<>>
        /\ poppedSeq = <<>>
        /\ destroyed = <<>>
        (* Process Prod *)
        /\ n = 0
        /\ cur_ = 0
        /\ nxt = 0
        (* Process Cons *)
        /\ m = 0
        /\ cur = 0
        /\ pc = [self \in ProcSet |-> CASE self = 0 -> "U"
                                        [] self = 1 -> "O"]

U == /\ pc[0] = "U"
     /\ IF n < Pushes
           THEN /\ n' = n + 1
                /\ pc' = [pc EXCEPT ![0] = "U1"]
           ELSE /\ pc' = [pc EXCEPT ![0] = "Done"]
                /\ n' = n
     /\ UNCHANGED << w, r, slot, writing, reading, pushedSeq, poppedSeq, 
                     destroyed, cur_, nxt, m, cur >>

U1 == /\ pc[0] = "U1"
      /\ cur_' = w
      /\ nxt' = (w + 1) % Size
      /\ pc' = [pc EXCEPT ![0] = "U2"]
      /\ UNCHANGED << w, r, slot, writing, reading, pushedSeq, poppedSeq, 
                      destroyed, n, m, cur >>

U2 == /\ pc[0] = "U2"
      /\ IF nxt # r
            THEN /\ writing' = cur_
                 /\ pc' = [pc EXCEPT ![0] = "U3"]
            ELSE /\ pc' = [pc EXCEPT ![0] = "U"]
                 /\ UNCHANGED writing
      /\ UNCHANGED << w, r, slot, reading, pushedSeq, poppedSeq, destroyed, n, 
                      cur_, nxt, m, cur >>

U3 == /\ pc[0] = "U3"
      /\ Assert(reading # cur_, 
                "Failure of assertion at line 35, column 13.")
      /\ slot' = [slot EXCEPT ![cur_] = n]
      /\ pc' = [pc EXCEPT ![0] = "U4"]
      /\ UNCHANGED << w, r, writing, reading, pushedSeq, poppedSeq, destroyed, 
                      n, cur_, nxt, m, cur >>

U4 == /\ pc[0] = "U4"
      /\ writing' = -1
      /\ w' = nxt
      /\ pushedSeq' = Append(pushedSeq, n)
      /\ pc' = [pc EXCEPT ![0] = "U"]
      /\ UNCHANGED << r, slot, reading, poppedSeq, destroyed, n, cur_, nxt, m, 
                      cur >>

Prod == U \/ U1 \/ U2 \/ U3 \/ U4

O == /\ pc[1] = "O"
     /\ IF m < Pops
           THEN /\ m' = m + 1
                /\ pc' = [pc EXCEPT ![1] = "O1"]
           ELSE /\ pc' = [pc EXCEPT ![1] = "D"]
                /\ m' = m
     /\ UNCHANGED << w, r, slot, writing, reading, pushedSeq, poppedSeq, 
                     destroyed, n, cur_, nxt, cur >>

O1 == /\ pc[1] = "O1"
      /\ cur' = r
      /\ pc' = [pc EXCEPT ![1] = "O2"]
      /\ UNCHANGED << w, r, slot, writing, reading, pushedSeq, poppedSeq, 
                      destroyed, n, cur_, nxt, m >>

O2 == /\ pc[1] = "O2"
      /\ IF cur # w
            THEN /\ reading' = cur
                 /\ pc' = [pc EXCEPT ![1] = "O3"]
            ELSE /\ pc' = [pc EXCEPT ![1] = "O"]
                 /\ UNCHANGED reading
      /\ UNCHANGED << w, r, slot, writing, pushedSeq, poppedSeq, destroyed, n, 
                      cur_, nxt, m, cur >>

O3 == /\ pc[1] = "O3"
      /\ Assert(writing # cur, "Failure of assertion at line 51, column 13.")
      /\ poppedSeq' = Append(poppedSeq, slot[cur])
      /\ slot' = [slot EXCEPT ![cur] = 0]
      /\ pc' = [pc EXCEPT ![1] = "O4"]
      /\ UNCHANGED << w, r, writing, reading, pushedSeq, destroyed, n, cur_, 
                      nxt, m, cur >>

O4 == /\ pc[1] = "O4"
      /\ reading' = -1
      /\ r' = (cur + 1) % Size
      /\ pc' = [pc EXCEPT ![1] = "O"]
      /\ UNCHANGED << w, slot, writing, pushedSeq, poppedSeq, destroyed, n, 
                      cur_, nxt, m, cur >>

D == /\ pc[1] = "D"
     /\ pc[0] = "Done"
     /\ pc' = [pc EXCEPT ![1] = "D_loop"]
     /\ UNCHANGED << w, r, slot, writing, reading, pushedSeq, poppedSeq, 
                     destroyed, n, cur_, nxt, m, cur >>

D_loop == /\ pc[1] = "D_loop"
          /\ IF r # w
                THEN /\ destroyed' = Append(destroyed, slot[r])
                     /\ slot' = [slot EXCEPT ![r] = 0]
                     /\ r' = (r + 1) % Size
                     /\ pc' = [pc EXCEPT ![1] = "D_loop"]
                ELSE /\ pc' = [pc EXCEPT ![1] = "Done"]
                     /\ UNCHANGED << r, slot, destroyed >>
          /\ UNCHANGED << w, writing, reading, pushedSeq, poppedSeq, n, cur_, 
                          nxt, m, cur >>

Cons == O \/ O1 \/ O2 \/ O3 \/ O4 \/ D \/ D_loop

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == Prod \/ Cons
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ WF_vars(Prod)
        /\ WF_vars(Cons)

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* FIFO and nothing lost: popped, then destroyed, is exactly what was pushed, in order.
Fifo == \A i \in 1..Len(poppedSeq) : poppedSeq[i] = pushedSeq[i]
AllAccounted == (\A p \in ProcSet : pc[p] = "Done") => poppedSeq \o destroyed = pushedSeq
Safety == Fifo /\ AllAccounted
=============================================================================
