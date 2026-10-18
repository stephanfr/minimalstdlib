---------------------------- MODULE LockfreeBitset ----------------------------
(***************************************************************************)
(* include/lockfree/bitset: acquire(i) and release(i) on one atomic word.  *)
(*                                                                         *)
(*   acquire: old = words_[w].fetch_or(mask, acquire);                     *)
(*            return (old & mask) ? ALREADY_ACQUIRED : SUCCESS;            *)
(*   release: old = words_[w].fetch_and(~mask, release);                   *)
(*            return (old & mask) ? SUCCESS : ALREADY_RELEASED;            *)
(*                                                                         *)
(* Contexts are CPU x interrupt depth: a handler can run on a CPU between  *)
(* any two steps of the code it interrupted, and only the innermost        *)
(* context of a CPU runs.  Every context may try to acquire any bit (and   *)
(* may fail); it releases only bits it owns, as callers must.  Ownership   *)
(* comes from the value each RMW returns, so the invariants check what     *)
(* callers rely on rather than restating the transitions.                  *)
(*                                                                         *)
(* Bits of different words never interact, so one word is enough.          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    NumBits,            \* bits in the modelled word
    NumCPUs,
    MaxInterruptDepth   \* 0 = no interrupts

ASSUME NumBits \in 1..4 /\ NumCPUs \in 1..3 /\ MaxInterruptDepth \in 0..2

CPUs == 1..NumCPUs
Bits == 1..NumBits
Depths == 0..MaxInterruptDepth
Contexts == CPUs \X Depths

VARIABLES
    bits,           \* the atomic word
    activeDepth,    \* per CPU: depth of the running context
    owners,         \* per bit: contexts whose acquire returned SUCCESS and that have not released it
    lastRelease     \* ghost: result of the last release by an owner ("ok" / "failed" / "none")

vars == <<bits, activeDepth, owners, lastRelease>>

Running(ctx) == ctx[2] = activeDepth[ctx[1]]

TypeOK ==
    /\ bits \in [Bits -> BOOLEAN]
    /\ activeDepth \in [CPUs -> Depths]
    /\ owners \in [Bits -> SUBSET Contexts]
    /\ lastRelease \in {"none", "ok", "failed"}

Init ==
    /\ bits = [b \in Bits |-> FALSE]
    /\ activeDepth = [c \in CPUs |-> 0]
    /\ owners = [b \in Bits |-> {}]
    /\ lastRelease = "none"

-----------------------------------------------------------------------------
(* One atomic RMW each                                                     *)

Acquire(ctx, b) ==
    /\ Running(ctx)
    /\ ctx \notin owners[b]
    /\ bits' = [bits EXCEPT ![b] = TRUE]                                  \* fetch_or
    /\ owners' = IF bits[b] THEN owners                                   \* old bit set: ALREADY_ACQUIRED
                 ELSE [owners EXCEPT ![b] = @ \cup {ctx}]                 \* old bit clear: SUCCESS
    /\ UNCHANGED <<activeDepth, lastRelease>>

Release(ctx, b) ==
    /\ Running(ctx)
    /\ ctx \in owners[b]
    /\ bits' = [bits EXCEPT ![b] = FALSE]                                 \* fetch_and
    /\ owners' = [owners EXCEPT ![b] = @ \ {ctx}]
    /\ lastRelease' = IF bits[b] THEN "ok" ELSE "failed"                  \* old bit set: SUCCESS
    /\ UNCHANGED activeDepth

-----------------------------------------------------------------------------
(* Interrupts: a handler starts on a CPU between any two steps, and the    *)
(* interrupted context resumes when it returns.                            *)

FireInterrupt(c) ==
    /\ activeDepth[c] < MaxInterruptDepth
    /\ activeDepth' = [activeDepth EXCEPT ![c] = @ + 1]
    /\ UNCHANGED <<bits, owners, lastRelease>>

ReturnFromInterrupt(c) ==
    /\ activeDepth[c] > 0
    /\ activeDepth' = [activeDepth EXCEPT ![c] = @ - 1]
    /\ UNCHANGED <<bits, owners, lastRelease>>

Next ==
    \/ \E ctx \in Contexts, b \in Bits : Acquire(ctx, b) \/ Release(ctx, b)
    \/ \E c \in CPUs : FireInterrupt(c) \/ ReturnFromInterrupt(c)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* What callers rely on                                                    *)

\* No two contexts hold the same bit
MutualExclusion == \A b \in Bits : Cardinality(owners[b]) <= 1

\* A held bit is set, and a set bit is held
OwnedImpliesSet == \A b \in Bits : owners[b] # {} => bits[b]
SetImpliesOwned == \A b \in Bits : bits[b] => owners[b] # {}

\* The owner's release finds its bit set (SUCCESS)
ReleaseByOwnerSucceeds == lastRelease # "failed"

Safety ==
    /\ TypeOK
    /\ MutualExclusion
    /\ OwnedImpliesSet
    /\ SetImpliesOwned
    /\ ReleaseByOwnerSucceeds

=============================================================================
