--------------------------- MODULE SpMcValueStack ---------------------------
(***************************************************************************)
(* include/lockfree/sp_mc_value_stack: state = (generation, count).        *)
(*   push (one producer): load state; slots[count].store(v);              *)
(*        CAS state -> (gen + 1, count + 1); retry from the refreshed      *)
(*        state on failure.                                               *)
(*   pop (any consumers): load state; v = slots[count - 1].load;          *)
(*        CAS state -> (gen, count - 1); retry on failure.                *)
(* Generation wraps at GenMod (2^32 in C++).                              *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS Capacity, Pushes, Consumers, PopsPer, GenMod

PRODUCER == 0

(* --algorithm SpMcValueStack {
variables
    gen = 0, count = 0,
    slots = [i \in 0..(Capacity - 1) |-> 0],
    pushed = {}, popped = {};

fair process (Prod = PRODUCER)
variables n = 0, og = 0, oc = 0;
{
 U: while (n < Pushes) {
        n := n + 1;
 U_load: og := gen; oc := count;
 U_try:  if (oc >= Capacity) { goto U; };
 U_slot: slots[oc] := n;
 U_cas:  if (gen = og /\ count = oc) {
            gen := (og + 1) % GenMod; count := oc + 1;
            pushed := pushed \cup {n};
         } else { og := gen; oc := count; goto U_try; };
    };
}

fair process (Cons \in Consumers)
variables m = 0, cg = 0, cc = 0, v = 0;
{
 O: while (m < PopsPer) {
        m := m + 1;
 O_load: cg := gen; cc := count;
 O_try:  if (cc = 0) { goto O; };
 O_slot: v := slots[cc - 1];
 O_cas:  if (gen = cg /\ count = cc) {
            count := cc - 1;
            assert v \in pushed /\ v \notin popped;      \* a live value, never popped twice
            popped := popped \cup {v};
         } else { cg := gen; cc := count; goto O_try; };
    };
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, gen, count, slots, pushed, popped, n, og, oc, m, cg, cc, v

vars == << pc, gen, count, slots, pushed, popped, n, og, oc, m, cg, cc, v >>

ProcSet == {PRODUCER} \cup (Consumers)

Init == (* Global variables *)
        /\ gen = 0
        /\ count = 0
        /\ slots = [i \in 0..(Capacity - 1) |-> 0]
        /\ pushed = {}
        /\ popped = {}
        (* Process Prod *)
        /\ n = 0
        /\ og = 0
        /\ oc = 0
        (* Process Cons *)
        /\ m = [self \in Consumers |-> 0]
        /\ cg = [self \in Consumers |-> 0]
        /\ cc = [self \in Consumers |-> 0]
        /\ v = [self \in Consumers |-> 0]
        /\ pc = [self \in ProcSet |-> CASE self = PRODUCER -> "U"
                                        [] self \in Consumers -> "O"]

U == /\ pc[PRODUCER] = "U"
     /\ IF n < Pushes
           THEN /\ n' = n + 1
                /\ pc' = [pc EXCEPT ![PRODUCER] = "U_load"]
           ELSE /\ pc' = [pc EXCEPT ![PRODUCER] = "Done"]
                /\ n' = n
     /\ UNCHANGED << gen, count, slots, pushed, popped, og, oc, m, cg, cc, v >>

U_load == /\ pc[PRODUCER] = "U_load"
          /\ og' = gen
          /\ oc' = count
          /\ pc' = [pc EXCEPT ![PRODUCER] = "U_try"]
          /\ UNCHANGED << gen, count, slots, pushed, popped, n, m, cg, cc, v >>

U_try == /\ pc[PRODUCER] = "U_try"
         /\ IF oc >= Capacity
               THEN /\ pc' = [pc EXCEPT ![PRODUCER] = "U"]
               ELSE /\ pc' = [pc EXCEPT ![PRODUCER] = "U_slot"]
         /\ UNCHANGED << gen, count, slots, pushed, popped, n, og, oc, m, cg, 
                         cc, v >>

U_slot == /\ pc[PRODUCER] = "U_slot"
          /\ slots' = [slots EXCEPT ![oc] = n]
          /\ pc' = [pc EXCEPT ![PRODUCER] = "U_cas"]
          /\ UNCHANGED << gen, count, pushed, popped, n, og, oc, m, cg, cc, v >>

U_cas == /\ pc[PRODUCER] = "U_cas"
         /\ IF gen = og /\ count = oc
               THEN /\ gen' = (og + 1) % GenMod
                    /\ count' = oc + 1
                    /\ pushed' = (pushed \cup {n})
                    /\ pc' = [pc EXCEPT ![PRODUCER] = "U"]
                    /\ UNCHANGED << og, oc >>
               ELSE /\ og' = gen
                    /\ oc' = count
                    /\ pc' = [pc EXCEPT ![PRODUCER] = "U_try"]
                    /\ UNCHANGED << gen, count, pushed >>
         /\ UNCHANGED << slots, popped, n, m, cg, cc, v >>

Prod == U \/ U_load \/ U_try \/ U_slot \/ U_cas

O(self) == /\ pc[self] = "O"
           /\ IF m[self] < PopsPer
                 THEN /\ m' = [m EXCEPT ![self] = m[self] + 1]
                      /\ pc' = [pc EXCEPT ![self] = "O_load"]
                 ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                      /\ m' = m
           /\ UNCHANGED << gen, count, slots, pushed, popped, n, og, oc, cg, 
                           cc, v >>

O_load(self) == /\ pc[self] = "O_load"
                /\ cg' = [cg EXCEPT ![self] = gen]
                /\ cc' = [cc EXCEPT ![self] = count]
                /\ pc' = [pc EXCEPT ![self] = "O_try"]
                /\ UNCHANGED << gen, count, slots, pushed, popped, n, og, oc, 
                                m, v >>

O_try(self) == /\ pc[self] = "O_try"
               /\ IF cc[self] = 0
                     THEN /\ pc' = [pc EXCEPT ![self] = "O"]
                     ELSE /\ pc' = [pc EXCEPT ![self] = "O_slot"]
               /\ UNCHANGED << gen, count, slots, pushed, popped, n, og, oc, m, 
                               cg, cc, v >>

O_slot(self) == /\ pc[self] = "O_slot"
                /\ v' = [v EXCEPT ![self] = slots[cc[self] - 1]]
                /\ pc' = [pc EXCEPT ![self] = "O_cas"]
                /\ UNCHANGED << gen, count, slots, pushed, popped, n, og, oc, 
                                m, cg, cc >>

O_cas(self) == /\ pc[self] = "O_cas"
               /\ IF gen = cg[self] /\ count = cc[self]
                     THEN /\ count' = cc[self] - 1
                          /\ Assert(v[self] \in pushed /\ v[self] \notin popped, 
                                    "Failure of assertion at line 48, column 13.")
                          /\ popped' = (popped \cup {v[self]})
                          /\ pc' = [pc EXCEPT ![self] = "O"]
                          /\ UNCHANGED << cg, cc >>
                     ELSE /\ cg' = [cg EXCEPT ![self] = gen]
                          /\ cc' = [cc EXCEPT ![self] = count]
                          /\ pc' = [pc EXCEPT ![self] = "O_try"]
                          /\ UNCHANGED << count, popped >>
               /\ UNCHANGED << gen, slots, pushed, n, og, oc, m, v >>

Cons(self) == O(self) \/ O_load(self) \/ O_try(self) \/ O_slot(self)
                 \/ O_cas(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == Prod
           \/ (\E self \in Consumers: Cons(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ WF_vars(Prod)
        /\ \A self \in Consumers : WF_vars(Cons(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* What is still stacked is exactly what was pushed and not popped.
StackContents == {slots[i] : i \in 0..(count - 1)}
Conserved == (\A p \in ProcSet : pc[p] = "Done") => StackContents = pushed \ popped
Safety == popped \subseteq pushed /\ Conserved
=============================================================================
