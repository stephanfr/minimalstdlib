-------------------------- MODULE EliminationArray --------------------------
(***************************************************************************)
(* include/lockfree/elimination_array: one slot's state machine.           *)
(*   put:  CAS EMPTY->PUT_SETUP; element.store; state.store(WAITING_GET);  *)
(*         CAS WAITING_GET->EMPTY (cancel, return false) or spin until     *)
(*         CAS ELIMINATED->EMPTY (return true).                            *)
(*         Else CAS WAITING_PUT->ELIMINATING_PUT; element.store;           *)
(*         state.store(ELIMINATED) (return true).                          *)
(*   get:  CAS EMPTY->WAITING_PUT; CAS WAITING_PUT->EMPTY (cancel) or spin *)
(*         until CAS ELIMINATED->GET_CLEANUP; element.load; element.store  *)
(*         (null); state.store(EMPTY) (return the element).                *)
(*         Else CAS WAITING_GET->ELIMINATING_GET; element.load;            *)
(*         element.store(null); state.store(ELIMINATED).                   *)
(* The C++ tries 3 random slots; one slot with repeated attempts covers    *)
(* every interleaving on a slot (slots are independent).                   *)
(*                                                                         *)
(* SameCpuIrq = TRUE: thread 1 and thread 2 share a CPU and thread 2 is an  *)
(* interrupt handler: it runs only while thread 1 is not between steps it  *)
(* cannot leave (modelled as: while thread 2 is active, thread 1 does not  *)
(* run).  Result: still terminates.  The handler only spins after its own *)
(* CAS installed WAITING_GET/WAITING_PUT; the thread it interrupted froze  *)
(* before that install, so it is never the partner being waited on, and a  *)
(* slot it left mid-transition just makes the handler's CAS fail.          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS Threads, OpsPer, SameCpuIrq

(* --algorithm EliminationArray {
variables
    state = "EMPTY", element = 0,
    transferred = {}, received = {},       \* values whose put returned true / that a get returned
    irqActive = FALSE;

define {
    Val(t, k) == 10 * t + k
    CanRun(t) == ~(SameCpuIrq /\ t = 1 /\ irqActive)
    NoGhost == received \subseteq transferred \cup {Val(t, k) : t \in Threads, k \in 1..OpsPer}
}

fair process (T \in Threads)
variables k = 0, v = 0, got = 0;
{
 L: while (k < OpsPer) {
        await CanRun(self);
        k := k + 1;
        if (SameCpuIrq /\ self = 2) { irqActive := TRUE; };
        either {
            \* -------- put(Val(self, k))
            v := Val(self, k);
 P1:        await CanRun(self);
            if (state = "EMPTY") {
                state := "PUT_SETUP";
 P2:            await CanRun(self); element := v;
 P3:            await CanRun(self); state := "WAITING_GET";
 P4:            await CanRun(self);
                if (state = "WAITING_GET") { state := "EMPTY"; goto Done1; };
 P5:            await CanRun(self) /\ state = "ELIMINATED";       \* spin: CAS ELIMINATED -> EMPTY
                state := "EMPTY";
                transferred := transferred \cup {v};
                goto Done1;
            } else if (state = "WAITING_PUT") {
                state := "ELIMINATING_PUT";
 P6:            await CanRun(self); element := v;
 P7:            await CanRun(self); state := "ELIMINATED";
                transferred := transferred \cup {v};
                goto Done1;
            };
        } or {
            \* -------- get
 G1:        await CanRun(self);
            if (state = "EMPTY") {
                state := "WAITING_PUT";
 G2:            await CanRun(self);
                if (state = "WAITING_PUT") { state := "EMPTY"; goto Done1; };
 G3:            await CanRun(self) /\ state = "ELIMINATED";       \* spin: CAS ELIMINATED -> GET_CLEANUP
                state := "GET_CLEANUP";
 G4:            await CanRun(self); got := element;
 G5:            await CanRun(self); element := 0;
 G6:            await CanRun(self); state := "EMPTY";
                assert got # 0 /\ got \notin received;
                received := received \cup {got};
                goto Done1;
            } else if (state = "WAITING_GET") {
                state := "ELIMINATING_GET";
 G7:            await CanRun(self); got := element;
 G8:            await CanRun(self); element := 0;
 G9:            await CanRun(self); state := "ELIMINATED";
                assert got # 0 /\ got \notin received;
                received := received \cup {got};
                goto Done1;
            };
        };
 Done1: if (SameCpuIrq /\ self = 2) { irqActive := FALSE; };
    };
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, state, element, transferred, received, irqActive

(* define statement *)
Val(t, k) == 10 * t + k
CanRun(t) == ~(SameCpuIrq /\ t = 1 /\ irqActive)
NoGhost == received \subseteq transferred \cup {Val(t, k) : t \in Threads, k \in 1..OpsPer}

VARIABLES k, v, got

vars == << pc, state, element, transferred, received, irqActive, k, v, got >>

ProcSet == (Threads)

Init == (* Global variables *)
        /\ state = "EMPTY"
        /\ element = 0
        /\ transferred = {}
        /\ received = {}
        /\ irqActive = FALSE
        (* Process T *)
        /\ k = [self \in Threads |-> 0]
        /\ v = [self \in Threads |-> 0]
        /\ got = [self \in Threads |-> 0]
        /\ pc = [self \in ProcSet |-> "L"]

L(self) == /\ pc[self] = "L"
           /\ IF k[self] < OpsPer
                 THEN /\ CanRun(self)
                      /\ k' = [k EXCEPT ![self] = k[self] + 1]
                      /\ IF SameCpuIrq /\ self = 2
                            THEN /\ irqActive' = TRUE
                            ELSE /\ TRUE
                                 /\ UNCHANGED irqActive
                      /\ \/ /\ v' = [v EXCEPT ![self] = Val(self, k'[self])]
                            /\ pc' = [pc EXCEPT ![self] = "P1"]
                         \/ /\ pc' = [pc EXCEPT ![self] = "G1"]
                            /\ v' = v
                 ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                      /\ UNCHANGED << irqActive, k, v >>
           /\ UNCHANGED << state, element, transferred, received, got >>

Done1(self) == /\ pc[self] = "Done1"
               /\ IF SameCpuIrq /\ self = 2
                     THEN /\ irqActive' = FALSE
                     ELSE /\ TRUE
                          /\ UNCHANGED irqActive
               /\ pc' = [pc EXCEPT ![self] = "L"]
               /\ UNCHANGED << state, element, transferred, received, k, v, 
                               got >>

P1(self) == /\ pc[self] = "P1"
            /\ CanRun(self)
            /\ IF state = "EMPTY"
                  THEN /\ state' = "PUT_SETUP"
                       /\ pc' = [pc EXCEPT ![self] = "P2"]
                  ELSE /\ IF state = "WAITING_PUT"
                             THEN /\ state' = "ELIMINATING_PUT"
                                  /\ pc' = [pc EXCEPT ![self] = "P6"]
                             ELSE /\ pc' = [pc EXCEPT ![self] = "Done1"]
                                  /\ state' = state
            /\ UNCHANGED << element, transferred, received, irqActive, k, v, 
                            got >>

P2(self) == /\ pc[self] = "P2"
            /\ CanRun(self)
            /\ element' = v[self]
            /\ pc' = [pc EXCEPT ![self] = "P3"]
            /\ UNCHANGED << state, transferred, received, irqActive, k, v, got >>

P3(self) == /\ pc[self] = "P3"
            /\ CanRun(self)
            /\ state' = "WAITING_GET"
            /\ pc' = [pc EXCEPT ![self] = "P4"]
            /\ UNCHANGED << element, transferred, received, irqActive, k, v, 
                            got >>

P4(self) == /\ pc[self] = "P4"
            /\ CanRun(self)
            /\ IF state = "WAITING_GET"
                  THEN /\ state' = "EMPTY"
                       /\ pc' = [pc EXCEPT ![self] = "Done1"]
                  ELSE /\ pc' = [pc EXCEPT ![self] = "P5"]
                       /\ state' = state
            /\ UNCHANGED << element, transferred, received, irqActive, k, v, 
                            got >>

P5(self) == /\ pc[self] = "P5"
            /\ CanRun(self) /\ state = "ELIMINATED"
            /\ state' = "EMPTY"
            /\ transferred' = (transferred \cup {v[self]})
            /\ pc' = [pc EXCEPT ![self] = "Done1"]
            /\ UNCHANGED << element, received, irqActive, k, v, got >>

P6(self) == /\ pc[self] = "P6"
            /\ CanRun(self)
            /\ element' = v[self]
            /\ pc' = [pc EXCEPT ![self] = "P7"]
            /\ UNCHANGED << state, transferred, received, irqActive, k, v, got >>

P7(self) == /\ pc[self] = "P7"
            /\ CanRun(self)
            /\ state' = "ELIMINATED"
            /\ transferred' = (transferred \cup {v[self]})
            /\ pc' = [pc EXCEPT ![self] = "Done1"]
            /\ UNCHANGED << element, received, irqActive, k, v, got >>

G1(self) == /\ pc[self] = "G1"
            /\ CanRun(self)
            /\ IF state = "EMPTY"
                  THEN /\ state' = "WAITING_PUT"
                       /\ pc' = [pc EXCEPT ![self] = "G2"]
                  ELSE /\ IF state = "WAITING_GET"
                             THEN /\ state' = "ELIMINATING_GET"
                                  /\ pc' = [pc EXCEPT ![self] = "G7"]
                             ELSE /\ pc' = [pc EXCEPT ![self] = "Done1"]
                                  /\ state' = state
            /\ UNCHANGED << element, transferred, received, irqActive, k, v, 
                            got >>

G2(self) == /\ pc[self] = "G2"
            /\ CanRun(self)
            /\ IF state = "WAITING_PUT"
                  THEN /\ state' = "EMPTY"
                       /\ pc' = [pc EXCEPT ![self] = "Done1"]
                  ELSE /\ pc' = [pc EXCEPT ![self] = "G3"]
                       /\ state' = state
            /\ UNCHANGED << element, transferred, received, irqActive, k, v, 
                            got >>

G3(self) == /\ pc[self] = "G3"
            /\ CanRun(self) /\ state = "ELIMINATED"
            /\ state' = "GET_CLEANUP"
            /\ pc' = [pc EXCEPT ![self] = "G4"]
            /\ UNCHANGED << element, transferred, received, irqActive, k, v, 
                            got >>

G4(self) == /\ pc[self] = "G4"
            /\ CanRun(self)
            /\ got' = [got EXCEPT ![self] = element]
            /\ pc' = [pc EXCEPT ![self] = "G5"]
            /\ UNCHANGED << state, element, transferred, received, irqActive, 
                            k, v >>

G5(self) == /\ pc[self] = "G5"
            /\ CanRun(self)
            /\ element' = 0
            /\ pc' = [pc EXCEPT ![self] = "G6"]
            /\ UNCHANGED << state, transferred, received, irqActive, k, v, got >>

G6(self) == /\ pc[self] = "G6"
            /\ CanRun(self)
            /\ state' = "EMPTY"
            /\ Assert(got[self] # 0 /\ got[self] \notin received, 
                      "Failure of assertion at line 79, column 17.")
            /\ received' = (received \cup {got[self]})
            /\ pc' = [pc EXCEPT ![self] = "Done1"]
            /\ UNCHANGED << element, transferred, irqActive, k, v, got >>

G7(self) == /\ pc[self] = "G7"
            /\ CanRun(self)
            /\ got' = [got EXCEPT ![self] = element]
            /\ pc' = [pc EXCEPT ![self] = "G8"]
            /\ UNCHANGED << state, element, transferred, received, irqActive, 
                            k, v >>

G8(self) == /\ pc[self] = "G8"
            /\ CanRun(self)
            /\ element' = 0
            /\ pc' = [pc EXCEPT ![self] = "G9"]
            /\ UNCHANGED << state, transferred, received, irqActive, k, v, got >>

G9(self) == /\ pc[self] = "G9"
            /\ CanRun(self)
            /\ state' = "ELIMINATED"
            /\ Assert(got[self] # 0 /\ got[self] \notin received, 
                      "Failure of assertion at line 87, column 17.")
            /\ received' = (received \cup {got[self]})
            /\ pc' = [pc EXCEPT ![self] = "Done1"]
            /\ UNCHANGED << element, transferred, irqActive, k, v, got >>

T(self) == L(self) \/ Done1(self) \/ P1(self) \/ P2(self) \/ P3(self)
              \/ P4(self) \/ P5(self) \/ P6(self) \/ P7(self) \/ G1(self)
              \/ G2(self) \/ G3(self) \/ G4(self) \/ G5(self) \/ G6(self)
              \/ G7(self) \/ G8(self) \/ G9(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Threads: T(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Threads : WF_vars(T(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* Only real elements are received (never twice: asserted at G6/G9), and once every thread is done the
\*   elements received are exactly those whose put returned true.  (In between, a get can return before the
\*   put's spin has recorded its success.)
AllVals == {Val(t, j) : t \in Threads, j \in 1..OpsPer}
ExactlyOnce == /\ received \subseteq AllVals
               /\ (\A t \in ProcSet : pc[t] = "Done") => received = transferred
Safety == ExactlyOnce
=============================================================================
