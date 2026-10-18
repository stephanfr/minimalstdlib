------------------------ MODULE IteratorSavedHandle ------------------------
(***************************************************************************)
(* include/lockfree/skiplist iterator::operator++ (and const_iterator's):  *)
(* an iterator keeps its node's handle between calls, outside any read     *)
(* section, and resolves it in a new section.                              *)
(*                                                                         *)
(*   remover   final unlink; retire_node: mark_retired; fence;             *)
(*             retired_epoch_ = epoch                                      *)
(*   reclaimer try_advance_epoch: advance when every active reader's       *)
(*             snapshot is current; reclaim: free when retired_epoch_ <=   *)
(*             epoch - 2 and every active reader's snapshot is above it;   *)
(*             ~skip_node: free_slot (table entry = null), then the memory *)
(*   iterator  enter section (snapshot = epoch);                           *)
(*             p = unpack_unretired_ptr(handle): null if the retired mark  *)
(*             is set, and the iterator leapfrogs; read p's links; exit    *)
(*                                                                         *)
(* The orderings are sequentially consistent here; on weak memory they    *)
(* rest on the fences (litmus/aarch64 K).                                  *)
(***************************************************************************)
EXTENDS Integers, TLC

CONSTANTS MaxEpoch, Passes

(* --algorithm IteratorSavedHandle {
variables
    epoch = 1, node = "linked", retiredEpoch = 0, marked = FALSE, table = 1,
    active = FALSE, snapshot = 0;

fair process (Remover = 1)
{
 U_unlink:  node := "unlinked";
 U_mark:    marked := TRUE;
 U_stamp:   retiredEpoch := epoch; node := "retired";
}

fair process (Reclaimer = 2)
{
 C_loop:
    while (epoch < MaxEpoch \/ node # "freed") {
        either {
            await (epoch < MaxEpoch \/ (node = "retired" /\ epoch < retiredEpoch + 2)) /\ (active => snapshot >= epoch);
            epoch := epoch + 1;
        } or {
            await node = "retired" /\ retiredEpoch <= epoch - 2 /\ (active => snapshot > epoch - 2);
            node := "destroying";
 C_slot:    table := 0;                                \* ~skip_node: free_slot
 C_free:    node := "freed";
        };
    };
}

fair process (Iterator = 3)
variables n = 0, p = 0;
{
 I_loop:
    while (n < Passes) {
        n := n + 1;
 I_enter:   active := TRUE; snapshot := epoch;
 I_resolve: p := table;
            if (p # 0) {
 I_check:       if (marked) { p := 0; };
 I_read:        if (p # 0) { assert node # "freed" /\ node # "destroying"; };
            };
 I_exit:    active := FALSE;
    };
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, epoch, node, retiredEpoch, marked, table, active, snapshot, n, 
          p

vars == << pc, epoch, node, retiredEpoch, marked, table, active, snapshot, n, 
           p >>

ProcSet == {1} \cup {2} \cup {3}

Init == (* Global variables *)
        /\ epoch = 1
        /\ node = "linked"
        /\ retiredEpoch = 0
        /\ marked = FALSE
        /\ table = 1
        /\ active = FALSE
        /\ snapshot = 0
        (* Process Iterator *)
        /\ n = 0
        /\ p = 0
        /\ pc = [self \in ProcSet |-> CASE self = 1 -> "U_unlink"
                                        [] self = 2 -> "C_loop"
                                        [] self = 3 -> "I_loop"]

U_unlink == /\ pc[1] = "U_unlink"
            /\ node' = "unlinked"
            /\ pc' = [pc EXCEPT ![1] = "U_mark"]
            /\ UNCHANGED << epoch, retiredEpoch, marked, table, active, 
                            snapshot, n, p >>

U_mark == /\ pc[1] = "U_mark"
          /\ marked' = TRUE
          /\ pc' = [pc EXCEPT ![1] = "U_stamp"]
          /\ UNCHANGED << epoch, node, retiredEpoch, table, active, snapshot, 
                          n, p >>

U_stamp == /\ pc[1] = "U_stamp"
           /\ retiredEpoch' = epoch
           /\ node' = "retired"
           /\ pc' = [pc EXCEPT ![1] = "Done"]
           /\ UNCHANGED << epoch, marked, table, active, snapshot, n, p >>

Remover == U_unlink \/ U_mark \/ U_stamp

C_loop == /\ pc[2] = "C_loop"
          /\ IF epoch < MaxEpoch \/ node # "freed"
                THEN /\ \/ /\ (epoch < MaxEpoch \/ (node = "retired" /\ epoch < retiredEpoch + 2)) /\ (active => snapshot >= epoch)
                           /\ epoch' = epoch + 1
                           /\ pc' = [pc EXCEPT ![2] = "C_loop"]
                           /\ node' = node
                        \/ /\ node = "retired" /\ retiredEpoch <= epoch - 2 /\ (active => snapshot > epoch - 2)
                           /\ node' = "destroying"
                           /\ pc' = [pc EXCEPT ![2] = "C_slot"]
                           /\ epoch' = epoch
                ELSE /\ pc' = [pc EXCEPT ![2] = "Done"]
                     /\ UNCHANGED << epoch, node >>
          /\ UNCHANGED << retiredEpoch, marked, table, active, snapshot, n, p >>

C_slot == /\ pc[2] = "C_slot"
          /\ table' = 0
          /\ pc' = [pc EXCEPT ![2] = "C_free"]
          /\ UNCHANGED << epoch, node, retiredEpoch, marked, active, snapshot, 
                          n, p >>

C_free == /\ pc[2] = "C_free"
          /\ node' = "freed"
          /\ pc' = [pc EXCEPT ![2] = "C_loop"]
          /\ UNCHANGED << epoch, retiredEpoch, marked, table, active, snapshot, 
                          n, p >>

Reclaimer == C_loop \/ C_slot \/ C_free

I_loop == /\ pc[3] = "I_loop"
          /\ IF n < Passes
                THEN /\ n' = n + 1
                     /\ pc' = [pc EXCEPT ![3] = "I_enter"]
                ELSE /\ pc' = [pc EXCEPT ![3] = "Done"]
                     /\ n' = n
          /\ UNCHANGED << epoch, node, retiredEpoch, marked, table, active, 
                          snapshot, p >>

I_enter == /\ pc[3] = "I_enter"
           /\ active' = TRUE
           /\ snapshot' = epoch
           /\ pc' = [pc EXCEPT ![3] = "I_resolve"]
           /\ UNCHANGED << epoch, node, retiredEpoch, marked, table, n, p >>

I_resolve == /\ pc[3] = "I_resolve"
             /\ p' = table
             /\ IF p' # 0
                   THEN /\ pc' = [pc EXCEPT ![3] = "I_check"]
                   ELSE /\ pc' = [pc EXCEPT ![3] = "I_exit"]
             /\ UNCHANGED << epoch, node, retiredEpoch, marked, table, active, 
                             snapshot, n >>

I_check == /\ pc[3] = "I_check"
           /\ IF marked
                 THEN /\ p' = 0
                 ELSE /\ TRUE
                      /\ p' = p
           /\ pc' = [pc EXCEPT ![3] = "I_read"]
           /\ UNCHANGED << epoch, node, retiredEpoch, marked, table, active, 
                           snapshot, n >>

I_read == /\ pc[3] = "I_read"
          /\ IF p # 0
                THEN /\ Assert(node # "freed" /\ node # "destroying", 
                               "Failure of assertion at line 62, column 30.")
                ELSE /\ TRUE
          /\ pc' = [pc EXCEPT ![3] = "I_exit"]
          /\ UNCHANGED << epoch, node, retiredEpoch, marked, table, active, 
                          snapshot, n, p >>

I_exit == /\ pc[3] = "I_exit"
          /\ active' = FALSE
          /\ pc' = [pc EXCEPT ![3] = "I_loop"]
          /\ UNCHANGED << epoch, node, retiredEpoch, marked, table, snapshot, 
                          n, p >>

Iterator == I_loop \/ I_enter \/ I_resolve \/ I_check \/ I_read \/ I_exit

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == Remover \/ Reclaimer \/ Iterator
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ WF_vars(Remover)
        /\ WF_vars(Reclaimer)
        /\ WF_vars(Iterator)

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION
=============================================================================
