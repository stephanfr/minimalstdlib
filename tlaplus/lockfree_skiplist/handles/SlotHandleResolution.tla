------------------------ MODULE SlotHandleResolution ------------------------
(***************************************************************************)
(* include/lockfree/skiplist atomic_forward_link::unpack_ptr() against     *)
(* free_slot() and publish_slot() on the same slot.                        *)
(*                                                                         *)
(*   publish_slot: generation.store(gen + 1); slot_table.store(node)       *)
(*   free_slot:    slot_table.store(nullptr)                               *)
(*   unpack_ptr:   g = generation.load(); if g != handle.gen return null;  *)
(*                 p = slot_table.load();                                  *)
(*                 [Recheck: if generation.load() != handle.gen return     *)
(*                  null]; return p                                        *)
(*                                                                         *)
(* The node published in generation k is node k.  Readers hold handles of *)
(* every generation up to MaxGen, so some are current and some stale.      *)
(* Invariant: unpack_ptr returns null or the handle's own node.            *)
(***************************************************************************)
EXTENDS Integers, TLC

CONSTANTS MaxGen, Readers, Recheck

(* --algorithm SlotHandleResolution {
variables gen = 1, table = 1;

fair process (W = 0)
{
 W_loop:
    while (gen < MaxGen) {
 W_free:    table := 0;
 W_gen:     gen := gen + 1;
 W_table:   table := gen;
    };
}

fair process (R \in Readers)
variables h \in 1..MaxGen, g = 0, p = 0, res = -1;
{
 R_gen:     g := gen;
            if (g # h) { res := 0; goto R_done; };
 R_table:   p := table;
            if (Recheck) {
 R_recheck:     if (gen # h) { res := 0; goto R_done; };
            };
 R_ret:     res := p;
 R_done:    assert res \in {0, h};
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, gen, table, h, g, p, res

vars == << pc, gen, table, h, g, p, res >>

ProcSet == {0} \cup (Readers)

Init == (* Global variables *)
        /\ gen = 1
        /\ table = 1
        (* Process R *)
        /\ h \in [Readers -> 1..MaxGen]
        /\ g = [self \in Readers |-> 0]
        /\ p = [self \in Readers |-> 0]
        /\ res = [self \in Readers |-> -1]
        /\ pc = [self \in ProcSet |-> CASE self = 0 -> "W_loop"
                                        [] self \in Readers -> "R_gen"]

W_loop == /\ pc[0] = "W_loop"
          /\ IF gen < MaxGen
                THEN /\ pc' = [pc EXCEPT ![0] = "W_free"]
                ELSE /\ pc' = [pc EXCEPT ![0] = "Done"]
          /\ UNCHANGED << gen, table, h, g, p, res >>

W_free == /\ pc[0] = "W_free"
          /\ table' = 0
          /\ pc' = [pc EXCEPT ![0] = "W_gen"]
          /\ UNCHANGED << gen, h, g, p, res >>

W_gen == /\ pc[0] = "W_gen"
         /\ gen' = gen + 1
         /\ pc' = [pc EXCEPT ![0] = "W_table"]
         /\ UNCHANGED << table, h, g, p, res >>

W_table == /\ pc[0] = "W_table"
           /\ table' = gen
           /\ pc' = [pc EXCEPT ![0] = "W_loop"]
           /\ UNCHANGED << gen, h, g, p, res >>

W == W_loop \/ W_free \/ W_gen \/ W_table

R_gen(self) == /\ pc[self] = "R_gen"
               /\ g' = [g EXCEPT ![self] = gen]
               /\ IF g'[self] # h[self]
                     THEN /\ res' = [res EXCEPT ![self] = 0]
                          /\ pc' = [pc EXCEPT ![self] = "R_done"]
                     ELSE /\ pc' = [pc EXCEPT ![self] = "R_table"]
                          /\ res' = res
               /\ UNCHANGED << gen, table, h, p >>

R_table(self) == /\ pc[self] = "R_table"
                 /\ p' = [p EXCEPT ![self] = table]
                 /\ IF Recheck
                       THEN /\ pc' = [pc EXCEPT ![self] = "R_recheck"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "R_ret"]
                 /\ UNCHANGED << gen, table, h, g, res >>

R_recheck(self) == /\ pc[self] = "R_recheck"
                   /\ IF gen # h[self]
                         THEN /\ res' = [res EXCEPT ![self] = 0]
                              /\ pc' = [pc EXCEPT ![self] = "R_done"]
                         ELSE /\ pc' = [pc EXCEPT ![self] = "R_ret"]
                              /\ res' = res
                   /\ UNCHANGED << gen, table, h, g, p >>

R_ret(self) == /\ pc[self] = "R_ret"
               /\ res' = [res EXCEPT ![self] = p[self]]
               /\ pc' = [pc EXCEPT ![self] = "R_done"]
               /\ UNCHANGED << gen, table, h, g, p >>

R_done(self) == /\ pc[self] = "R_done"
                /\ Assert(res[self] \in {0, h[self]}, 
                          "Failure of assertion at line 44, column 13.")
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << gen, table, h, g, p, res >>

R(self) == R_gen(self) \/ R_table(self) \/ R_recheck(self) \/ R_ret(self)
              \/ R_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == W
           \/ (\E self \in Readers: R(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ WF_vars(W)
        /\ \A self \in Readers : WF_vars(R(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION
=============================================================================
