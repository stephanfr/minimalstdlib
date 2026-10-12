------------------------- MODULE SkiplistUpperLevels -------------------------
(***************************************************************************)
(* One skip-list node's upper levels, from the moment its level-0 link is  *)
(* published, through a concurrent remove, to its retirement.              *)
(* (include/lockfree/skiplist: insert(), remove(), the traversal helpers   *)
(* that unlink tombstoned nodes, and upper_levels_drained().)              *)
(*                                                                         *)
(* The hazard: an inserter is still publishing the node at an upper level  *)
(* while a remover tombstones it, unlinks level 0 and retires it.  If the  *)
(* inserter's CAS on a predecessor lands after that, a retired node is     *)
(* reachable at an upper level and is freed while still linked.           *)
(*                                                                         *)
(* The protocol (DrainGate = TRUE):                                        *)
(*   inserter, per upper level l:                                          *)
(*     upper_levels_remaining_ += 1; seq_cst fence      (reserve)         *)
(*     re-aim own link at l; abandon (-1) if it is tombstoned             *)
(*     CAS pred.next[l]: success publishes; failure -1 and retry/skip      *)
(*   remover: tombstone own links top-down, then level 0 (soft delete);   *)
(*     its traversal unlinks the upper links it finds (-1 each);          *)
(*     upper_levels_drained() (seq_cst fence; remaining <= 0)?            *)
(*       yes: unlink level 0, FULLY_UNLINKED, retire                       *)
(*       no:  leave level 0 linked                                         *)
(*   traversal helpers: unlink a tombstoned upper link (-1); if drained   *)
(*     and level 0 is tombstoned, unlink level 0 and retire.               *)
(* DrainGate = FALSE models the code before 68ab76f: no reservation; the  *)
(* remover unlinks the upper links it sees, then always unlinks level 0   *)
(* and retires, so a publish that lands after that pass is missed.       *)
(*                                                                         *)
(* RetargetBug = TRUE models the remove() before the retarget fix: after  *)
(* a failed level-0 mark its re-walk could land on this node (a re-insert *)
(* of a key whose original node another thread had removed) and mark its *)
(* level 0 without ever tombstoning its upper levels.  Those links are    *)
(* then never unlinked, the count never drains, and the node is never     *)
(* retired.  The fix restarts the remove, which is the normal remover     *)
(* (RetargetBug = FALSE).                                                  *)
(*                                                                         *)
(* TLA+ steps are sequentially consistent: the seq_cst fences in the C++  *)
(* are what make this hold on hardware.                                   *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Height,         \* the node's top level; upper levels are 1..Height-1
    NumHelpers,     \* concurrent traversals (find/remove/gc) that help unlink
    MaxCasFailures, \* upper-level CAS failures the inserter tolerates per level
    DrainGate,      \* TRUE: current code; FALSE: before 68ab76f
    RetargetBug     \* TRUE: the remover marks only level 0 (remove() before the retarget fix)

ASSUME Height \in 2..4 /\ NumHelpers \in 1..3 /\ MaxCasFailures \in 0..2 /\ DrainGate \in BOOLEAN
       /\ RetargetBug \in BOOLEAN

Upper == 1..(Height - 1)
INSERTER == 0
REMOVER == 1
Helpers == 2..(NumHelpers + 1)

(* --algorithm SkiplistUpperLevels {
variables
    linked = [l \in Upper |-> FALSE],          \* pred.next[l] = node (published at level l)
    ownMark = [l \in 0..(Height - 1) |-> FALSE], \* node.next[l] tombstoned
    level0Linked = TRUE,                        \* pred.next[0] = node
    remaining = 0,                              \* upper_levels_remaining_
    retired = FALSE,                            \* FULLY_UNLINKED and retire_node()
    retirements = 0;                            \* ghost: how many times it was retired

define {
    Drained == remaining <= 0
    UnlinkableUpper == {l \in Upper : linked[l] /\ ownMark[l]}
    \* When a traversal may unlink level 0 and retire.  Before 68ab76f a traversal descending top-down
    \*   retired once it had unlinked every tombstoned upper link it met (nothing left above).
    HelperMayRetire == IF DrainGate THEN Drained ELSE UnlinkableUpper = {}

    \* A retired node is unreachable at every level (else it is freed while linked)
    RetiredUnreachable == retired => (~level0Linked /\ \A l \in Upper : ~linked[l])
    RetiredOnce == retirements <= 1
    \* With the gate, every published link is still counted
    CountCoversLinks == DrainGate => remaining >= Cardinality({l \in Upper : linked[l]})
    Safety == RetiredUnreachable /\ RetiredOnce /\ CountCoversLinks
}

\* Unlink level 0 and retire, if it is still linked (the CAS on pred.next[0])
macro unlink_level0_and_retire() {
    if (level0Linked) {
        level0Linked := FALSE;
        retired := TRUE;
        retirements := retirements + 1;
    };
}

fair process (Inserter = INSERTER)
variables lvl = 1, failures = 0;
{
 I_level:
    while (lvl <= Height - 1) {
 I_reserve:
        if (DrainGate) { remaining := remaining + 1; };      \* fetch_add(1, seq_cst); fence
 I_own:
        if (ownMark[lvl]) {                                  \* own link tombstoned: a remove owns the node
            if (DrainGate) { remaining := remaining - 1; };
            goto I_done;
        };
 I_cas:
        either {
            linked[lvl] := TRUE;                             \* pred.next[lvl] CAS succeeds
            lvl := lvl + 1; failures := 0;
        } or {
            if (DrainGate) { remaining := remaining - 1; };  \* CAS failed: give the reservation back
            if (failures < MaxCasFailures) { failures := failures + 1; }
            else { lvl := lvl + 1; failures := 0; };         \* MAX_INSERT_UPPER_LEVEL_RETRIES: skip level
        };
    };
 I_done:
    skip;
}

fair process (Remover = REMOVER)
variables mlvl = Height - 1, rl = 0;
{
 R_mark:
    while (mlvl >= 1 /\ ~RetargetBug) {                     \* tombstone own upper links, top-down
        ownMark[mlvl] := TRUE;
        mlvl := mlvl - 1;
    };
 R_mark0:
    ownMark[0] := TRUE;                                      \* level-0 soft delete: this remove won
 R_unlink:
    while (UnlinkableUpper # {}) {                           \* its own traversal unlinks the marked upper links
        with (l \in UnlinkableUpper) { rl := l; };
        linked[rl] := FALSE;
        if (DrainGate) { remaining := remaining - 1; };
    };
 R_check:
    if (~DrainGate \/ Drained) {                             \* upper_levels_drained()
        unlink_level0_and_retire();
    };
}

fair process (Helper \in Helpers)
variables hl = 0;
{
 H_loop:
    while (TRUE) {
        either {
            await UnlinkableUpper # {};
            with (l \in UnlinkableUpper) { hl := l; };
 H_unlink:
            if (linked[hl]) {                                \* CAS pred.next[hl]: node -> node.next[hl]
                linked[hl] := FALSE;
 H_dec:
                if (DrainGate) { remaining := remaining - 1; };   \* fetch_sub(1); then upper_levels_drained()
 H_check:
                if (ownMark[0] /\ HelperMayRetire) { unlink_level0_and_retire(); };
            };
        } or {
            \* a traversal finding the node tombstoned at level 0 with its upper levels drained
            await ownMark[0] /\ level0Linked /\ HelperMayRetire;
 H_bottom:
            if (ownMark[0] /\ HelperMayRetire) { unlink_level0_and_retire(); };
        } or {
            \* no work left once the inserter and remover have finished
            await pc[INSERTER] = "Done" /\ pc[REMOVER] = "Done" /\ UnlinkableUpper = {}
                  /\ ~(ownMark[0] /\ level0Linked /\ HelperMayRetire);
            goto H_end;
        };
    };
 H_end:
    skip;
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, linked, ownMark, level0Linked, remaining, retired, retirements

(* define statement *)
Drained == remaining <= 0
UnlinkableUpper == {l \in Upper : linked[l] /\ ownMark[l]}


HelperMayRetire == IF DrainGate THEN Drained ELSE UnlinkableUpper = {}


RetiredUnreachable == retired => (~level0Linked /\ \A l \in Upper : ~linked[l])
RetiredOnce == retirements <= 1

CountCoversLinks == DrainGate => remaining >= Cardinality({l \in Upper : linked[l]})
Safety == RetiredUnreachable /\ RetiredOnce /\ CountCoversLinks

VARIABLES lvl, failures, mlvl, rl, hl

vars == << pc, linked, ownMark, level0Linked, remaining, retired, retirements, 
           lvl, failures, mlvl, rl, hl >>

ProcSet == {INSERTER} \cup {REMOVER} \cup (Helpers)

Init == (* Global variables *)
        /\ linked = [l \in Upper |-> FALSE]
        /\ ownMark = [l \in 0..(Height - 1) |-> FALSE]
        /\ level0Linked = TRUE
        /\ remaining = 0
        /\ retired = FALSE
        /\ retirements = 0
        (* Process Inserter *)
        /\ lvl = 1
        /\ failures = 0
        (* Process Remover *)
        /\ mlvl = Height - 1
        /\ rl = 0
        (* Process Helper *)
        /\ hl = [self \in Helpers |-> 0]
        /\ pc = [self \in ProcSet |-> CASE self = INSERTER -> "I_level"
                                        [] self = REMOVER -> "R_mark"
                                        [] self \in Helpers -> "H_loop"]

I_level == /\ pc[INSERTER] = "I_level"
           /\ IF lvl <= Height - 1
                 THEN /\ pc' = [pc EXCEPT ![INSERTER] = "I_reserve"]
                 ELSE /\ pc' = [pc EXCEPT ![INSERTER] = "I_done"]
           /\ UNCHANGED << linked, ownMark, level0Linked, remaining, retired, 
                           retirements, lvl, failures, mlvl, rl, hl >>

I_reserve == /\ pc[INSERTER] = "I_reserve"
             /\ IF DrainGate
                   THEN /\ remaining' = remaining + 1
                   ELSE /\ TRUE
                        /\ UNCHANGED remaining
             /\ pc' = [pc EXCEPT ![INSERTER] = "I_own"]
             /\ UNCHANGED << linked, ownMark, level0Linked, retired, 
                             retirements, lvl, failures, mlvl, rl, hl >>

I_own == /\ pc[INSERTER] = "I_own"
         /\ IF ownMark[lvl]
               THEN /\ IF DrainGate
                          THEN /\ remaining' = remaining - 1
                          ELSE /\ TRUE
                               /\ UNCHANGED remaining
                    /\ pc' = [pc EXCEPT ![INSERTER] = "I_done"]
               ELSE /\ pc' = [pc EXCEPT ![INSERTER] = "I_cas"]
                    /\ UNCHANGED remaining
         /\ UNCHANGED << linked, ownMark, level0Linked, retired, retirements, 
                         lvl, failures, mlvl, rl, hl >>

I_cas == /\ pc[INSERTER] = "I_cas"
         /\ \/ /\ linked' = [linked EXCEPT ![lvl] = TRUE]
               /\ lvl' = lvl + 1
               /\ failures' = 0
               /\ UNCHANGED remaining
            \/ /\ IF DrainGate
                     THEN /\ remaining' = remaining - 1
                     ELSE /\ TRUE
                          /\ UNCHANGED remaining
               /\ IF failures < MaxCasFailures
                     THEN /\ failures' = failures + 1
                          /\ lvl' = lvl
                     ELSE /\ lvl' = lvl + 1
                          /\ failures' = 0
               /\ UNCHANGED linked
         /\ pc' = [pc EXCEPT ![INSERTER] = "I_level"]
         /\ UNCHANGED << ownMark, level0Linked, retired, retirements, mlvl, rl, 
                         hl >>

I_done == /\ pc[INSERTER] = "I_done"
          /\ TRUE
          /\ pc' = [pc EXCEPT ![INSERTER] = "Done"]
          /\ UNCHANGED << linked, ownMark, level0Linked, remaining, retired, 
                          retirements, lvl, failures, mlvl, rl, hl >>

Inserter == I_level \/ I_reserve \/ I_own \/ I_cas \/ I_done

R_mark == /\ pc[REMOVER] = "R_mark"
          /\ IF mlvl >= 1 /\ ~RetargetBug
                THEN /\ ownMark' = [ownMark EXCEPT ![mlvl] = TRUE]
                     /\ mlvl' = mlvl - 1
                     /\ pc' = [pc EXCEPT ![REMOVER] = "R_mark"]
                ELSE /\ pc' = [pc EXCEPT ![REMOVER] = "R_mark0"]
                     /\ UNCHANGED << ownMark, mlvl >>
          /\ UNCHANGED << linked, level0Linked, remaining, retired, 
                          retirements, lvl, failures, rl, hl >>

R_mark0 == /\ pc[REMOVER] = "R_mark0"
           /\ ownMark' = [ownMark EXCEPT ![0] = TRUE]
           /\ pc' = [pc EXCEPT ![REMOVER] = "R_unlink"]
           /\ UNCHANGED << linked, level0Linked, remaining, retired, 
                           retirements, lvl, failures, mlvl, rl, hl >>

R_unlink == /\ pc[REMOVER] = "R_unlink"
            /\ IF UnlinkableUpper # {}
                  THEN /\ \E l \in UnlinkableUpper:
                            rl' = l
                       /\ linked' = [linked EXCEPT ![rl'] = FALSE]
                       /\ IF DrainGate
                             THEN /\ remaining' = remaining - 1
                             ELSE /\ TRUE
                                  /\ UNCHANGED remaining
                       /\ pc' = [pc EXCEPT ![REMOVER] = "R_unlink"]
                  ELSE /\ pc' = [pc EXCEPT ![REMOVER] = "R_check"]
                       /\ UNCHANGED << linked, remaining, rl >>
            /\ UNCHANGED << ownMark, level0Linked, retired, retirements, lvl, 
                            failures, mlvl, hl >>

R_check == /\ pc[REMOVER] = "R_check"
           /\ IF ~DrainGate \/ Drained
                 THEN /\ IF level0Linked
                            THEN /\ level0Linked' = FALSE
                                 /\ retired' = TRUE
                                 /\ retirements' = retirements + 1
                            ELSE /\ TRUE
                                 /\ UNCHANGED << level0Linked, retired, 
                                                 retirements >>
                 ELSE /\ TRUE
                      /\ UNCHANGED << level0Linked, retired, retirements >>
           /\ pc' = [pc EXCEPT ![REMOVER] = "Done"]
           /\ UNCHANGED << linked, ownMark, remaining, lvl, failures, mlvl, rl, 
                           hl >>

Remover == R_mark \/ R_mark0 \/ R_unlink \/ R_check

H_loop(self) == /\ pc[self] = "H_loop"
                /\ \/ /\ UnlinkableUpper # {}
                      /\ \E l \in UnlinkableUpper:
                           hl' = [hl EXCEPT ![self] = l]
                      /\ pc' = [pc EXCEPT ![self] = "H_unlink"]
                   \/ /\ ownMark[0] /\ level0Linked /\ HelperMayRetire
                      /\ pc' = [pc EXCEPT ![self] = "H_bottom"]
                      /\ hl' = hl
                   \/ /\ pc[INSERTER] = "Done" /\ pc[REMOVER] = "Done" /\ UnlinkableUpper = {}
                         /\ ~(ownMark[0] /\ level0Linked /\ HelperMayRetire)
                      /\ pc' = [pc EXCEPT ![self] = "H_end"]
                      /\ hl' = hl
                /\ UNCHANGED << linked, ownMark, level0Linked, remaining, 
                                retired, retirements, lvl, failures, mlvl, rl >>

H_unlink(self) == /\ pc[self] = "H_unlink"
                  /\ IF linked[hl[self]]
                        THEN /\ linked' = [linked EXCEPT ![hl[self]] = FALSE]
                             /\ pc' = [pc EXCEPT ![self] = "H_dec"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "H_loop"]
                             /\ UNCHANGED linked
                  /\ UNCHANGED << ownMark, level0Linked, remaining, retired, 
                                  retirements, lvl, failures, mlvl, rl, hl >>

H_dec(self) == /\ pc[self] = "H_dec"
               /\ IF DrainGate
                     THEN /\ remaining' = remaining - 1
                     ELSE /\ TRUE
                          /\ UNCHANGED remaining
               /\ pc' = [pc EXCEPT ![self] = "H_check"]
               /\ UNCHANGED << linked, ownMark, level0Linked, retired, 
                               retirements, lvl, failures, mlvl, rl, hl >>

H_check(self) == /\ pc[self] = "H_check"
                 /\ IF ownMark[0] /\ HelperMayRetire
                       THEN /\ IF level0Linked
                                  THEN /\ level0Linked' = FALSE
                                       /\ retired' = TRUE
                                       /\ retirements' = retirements + 1
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << level0Linked, retired, 
                                                       retirements >>
                       ELSE /\ TRUE
                            /\ UNCHANGED << level0Linked, retired, retirements >>
                 /\ pc' = [pc EXCEPT ![self] = "H_loop"]
                 /\ UNCHANGED << linked, ownMark, remaining, lvl, failures, 
                                 mlvl, rl, hl >>

H_bottom(self) == /\ pc[self] = "H_bottom"
                  /\ IF ownMark[0] /\ HelperMayRetire
                        THEN /\ IF level0Linked
                                   THEN /\ level0Linked' = FALSE
                                        /\ retired' = TRUE
                                        /\ retirements' = retirements + 1
                                   ELSE /\ TRUE
                                        /\ UNCHANGED << level0Linked, retired, 
                                                        retirements >>
                        ELSE /\ TRUE
                             /\ UNCHANGED << level0Linked, retired, 
                                             retirements >>
                  /\ pc' = [pc EXCEPT ![self] = "H_loop"]
                  /\ UNCHANGED << linked, ownMark, remaining, lvl, failures, 
                                  mlvl, rl, hl >>

H_end(self) == /\ pc[self] = "H_end"
               /\ TRUE
               /\ pc' = [pc EXCEPT ![self] = "Done"]
               /\ UNCHANGED << linked, ownMark, level0Linked, remaining, 
                               retired, retirements, lvl, failures, mlvl, rl, 
                               hl >>

Helper(self) == H_loop(self) \/ H_unlink(self) \/ H_dec(self)
                   \/ H_check(self) \/ H_bottom(self) \/ H_end(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == Inserter \/ Remover
           \/ (\E self \in Helpers: Helper(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ WF_vars(Inserter)
        /\ WF_vars(Remover)
        /\ \A self \in Helpers : WF_vars(Helper(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* Once every process has finished, the removed node has been retired.
EventuallyRetired == <>(retired)
=============================================================================
