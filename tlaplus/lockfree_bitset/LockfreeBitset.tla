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
(* NonAtomic = TRUE splits each RMW into a load and a store (a mutation:   *)
(* what the code would be without the atomic RMW); MutualExclusion then    *)
(* fails, which shows the invariants can catch a race.                     *)
(*                                                                         *)
(* Bits of different words never interact, so one word is enough.          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    NumBits,            \* bits in the modelled word
    NumCPUs,
    MaxInterruptDepth,  \* 0 = no interrupts
    NonAtomic           \* TRUE: load-then-store instead of one RMW (mutation)

ASSUME NumBits \in 1..4 /\ NumCPUs \in 1..3 /\ MaxInterruptDepth \in 0..2 /\ NonAtomic \in BOOLEAN

CPUs == 1..NumCPUs
Bits == 1..NumBits
Depths == 0..MaxInterruptDepth
Contexts == CPUs \X Depths

VARIABLES
    bits,           \* the atomic word
    activeDepth,    \* per CPU: depth of the running context
    owners,         \* per bit: contexts whose acquire returned SUCCESS and that have not released it
    pending,        \* NonAtomic only: per context, the in-flight operation <<kind, bit, loaded value>> or <<>>
    lastRelease     \* ghost: result of the last release by an owner ("ok" / "failed" / "none")

vars == <<bits, activeDepth, owners, pending, lastRelease>>

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
    /\ pending = [ctx \in Contexts |-> <<>>]
    /\ lastRelease = "none"

-----------------------------------------------------------------------------
(* One atomic RMW each                                                     *)

Acquire(ctx, b) ==
    /\ ~NonAtomic
    /\ Running(ctx)
    /\ ctx \notin owners[b]
    /\ bits' = [bits EXCEPT ![b] = TRUE]                                  \* fetch_or
    /\ owners' = IF bits[b] THEN owners                                   \* old bit set: ALREADY_ACQUIRED
                 ELSE [owners EXCEPT ![b] = @ \cup {ctx}]                 \* old bit clear: SUCCESS
    /\ UNCHANGED <<activeDepth, pending, lastRelease>>

Release(ctx, b) ==
    /\ ~NonAtomic
    /\ Running(ctx)
    /\ ctx \in owners[b]
    /\ bits' = [bits EXCEPT ![b] = FALSE]                                 \* fetch_and
    /\ owners' = [owners EXCEPT ![b] = @ \ {ctx}]
    /\ lastRelease' = IF bits[b] THEN "ok" ELSE "failed"                  \* old bit set: SUCCESS
    /\ UNCHANGED <<activeDepth, pending>>

-----------------------------------------------------------------------------
(* Mutation: load, then store                                              *)

LoadForAcquire(ctx, b) ==
    /\ NonAtomic
    /\ Running(ctx)
    /\ pending[ctx] = <<>>
    /\ ctx \notin owners[b]
    /\ pending' = [pending EXCEPT ![ctx] = <<"acquire", b, bits[b]>>]
    /\ UNCHANGED <<bits, activeDepth, owners, lastRelease>>

LoadForRelease(ctx, b) ==
    /\ NonAtomic
    /\ Running(ctx)
    /\ pending[ctx] = <<>>
    /\ ctx \in owners[b]
    /\ pending' = [pending EXCEPT ![ctx] = <<"release", b, bits[b]>>]
    /\ UNCHANGED <<bits, activeDepth, owners, lastRelease>>

StoreBack(ctx) ==
    /\ NonAtomic
    /\ Running(ctx)
    /\ pending[ctx] # <<>>
    /\ LET kind == pending[ctx][1]
           b    == pending[ctx][2]
           old  == pending[ctx][3] IN
       IF kind = "acquire"
       THEN /\ bits' = [bits EXCEPT ![b] = TRUE]
            /\ owners' = IF old THEN owners ELSE [owners EXCEPT ![b] = @ \cup {ctx}]
            /\ UNCHANGED lastRelease
       ELSE /\ bits' = [bits EXCEPT ![b] = FALSE]
            /\ owners' = [owners EXCEPT ![b] = @ \ {ctx}]
            /\ lastRelease' = IF old THEN "ok" ELSE "failed"
    /\ pending' = [pending EXCEPT ![ctx] = <<>>]
    /\ UNCHANGED activeDepth

-----------------------------------------------------------------------------
(* Interrupts: a handler starts on a CPU between any two steps, and the    *)
(* interrupted context resumes when it returns.                            *)

FireInterrupt(c) ==
    /\ activeDepth[c] < MaxInterruptDepth
    /\ activeDepth' = [activeDepth EXCEPT ![c] = @ + 1]
    /\ UNCHANGED <<bits, owners, pending, lastRelease>>

ReturnFromInterrupt(c) ==
    /\ activeDepth[c] > 0
    /\ pending[<<c, activeDepth[c]>>] = <<>>          \* a handler finishes the operation it started
    /\ activeDepth' = [activeDepth EXCEPT ![c] = @ - 1]
    /\ UNCHANGED <<bits, owners, pending, lastRelease>>

Next ==
    \/ \E ctx \in Contexts, b \in Bits :
          \/ Acquire(ctx, b) \/ Release(ctx, b)
          \/ LoadForAcquire(ctx, b) \/ LoadForRelease(ctx, b)
    \/ \E ctx \in Contexts : StoreBack(ctx)
    \/ \E c \in CPUs : FireInterrupt(c) \/ ReturnFromInterrupt(c)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* What callers rely on                                                    *)

\* No two contexts hold the same bit
MutualExclusion == \A b \in Bits : Cardinality(owners[b]) <= 1

\* A held bit is set; a set bit is held, or is being released by its owner's in-flight store
OwnedImpliesSet == \A b \in Bits : owners[b] # {} => bits[b]
SetImpliesOwned == \A b \in Bits : bits[b] => owners[b] # {} \/ \E ctx \in Contexts : pending[ctx] # <<>> /\ pending[ctx][2] = b

\* The owner's release finds its bit set (SUCCESS)
ReleaseByOwnerSucceeds == lastRelease # "failed"

Safety ==
    /\ TypeOK
    /\ MutualExclusion
    /\ OwnedImpliesSet
    /\ SetImpliesOwned
    /\ ReleaseByOwnerSucceeds

=============================================================================
