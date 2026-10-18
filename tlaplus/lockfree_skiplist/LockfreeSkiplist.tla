-------------------------- MODULE LockfreeSkiplist --------------------------
(***************************************************************************)
(* Bounded model for include/lockfree/skiplist with per-CPU epoch read     *)
(* sections and lagged retired-node reclamation.                            *)
(*                                                                          *)
(* Matches the C++ control flow of insert, remove, find and the public    *)
(* advance_and_reclaim(), at the level of keys (a key stands for the node  *)
(* that currently holds it).                                               *)
(*                                                                          *)
(*  - `fullyUnlinked`: a node must be FULLY_UNLINKED before it can be      *)
(*    reclaimed (`unlink_state::FULLY_UNLINKED`).                          *)
(*  - `CanAdvanceEpoch` / `CanReclaim`: the per-slot reader checks of      *)
(*    `try_advance_epoch` and `reclaim_from_per_cpu_list`.                 *)
(*  - Interrupt re-entrancy: a CPU may enter a nested read section; only   *)
(*    the outermost entry records the epoch.                               *)
(*  - remove(): marking the upper levels deletes nothing; the level-0 mark *)
(*    CAS is the deletion and the call returns true right after it.  The   *)
(*    node is unlinked and retired then only if its upper levels have      *)
(*    drained, otherwise later by a traversal (find's assist).  A failed   *)
(*    mark retries a bounded number of times and then returns false; the  *)
(*    re-walk after a failure may restart the call (stale handle, or a     *)
(*    re-inserted node: see upper_levels/ for why it must restart).        *)
(*  - Abstractions: CAS contention is a nondeterministic choice; retries   *)
(*    are bounded by MaxRetryTicks; an insert that meets a not-yet-unlinked *)
(*    tombstone of its key is modelled as rejecting (the code links a new  *)
(*    node beside it), because a key cannot be present and tombstoned at   *)
(*    once at this level of abstraction.                                    *)
(***************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Keys,
    Threads,
    Cpus,
    MaxLevels,
    MaxRetryTicks,
    MaxEpoch,
    EpochLag,
    MaxReadDepth

VARIABLES
    present,          \* Keys logically present (not soft-deleted)
    tomb,             \* Keys soft-deleted (marked) but not yet physically unlinked
    retired,          \* Keys physically unlinked and on the retired list
    fullyUnlinked,    \* Keys with unlink_state::FULLY_UNLINKED set (upper levels cleared)
    retiredAt,        \* Epoch at which each key was retired
    globalEpoch,
    readerDepth,      \* Per-CPU re-entrant read-section nesting depth (0 = idle)
    readerEpoch,      \* Per-CPU: epoch snapshot at outermost enter_read_section
    pc,
    op,
    opKey,
    level,
    retryTick,
    height,           \* height of the node holding each key (chosen at insert)
    upMarked,         \* upper levels of that node already tombstoned by some remove()
    interfered,       \* per thread: another thread changed a link since this thread's last CAS failure
    finishParity,     \* ghost: flips each time any operation completes (global progress)
    removeMarks       \* ghost: level-0 marks made by each thread's current remove() call

ghost == <<finishParity, removeMarks>>
links == <<height, upMarked, interfered>>

vars == <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
          readerDepth, readerEpoch, pc, op, opKey, level, retryTick, height, upMarked, interfered,
          finishParity, removeMarks>>

PcStates == {
    "idle",
    "insert_find", "insert_link0", "insert_link_upper",
    "remove_find", "remove_mark", "remove_bottom",
    "find_scan"
}

Ops == {"none", "insert", "remove", "find"}

(*
 * In the C++ code each thread maps to the CPU it is currently scheduled on.
 * For simplicity in the bounded model we allow any mapping; the critical
 * property is that two threads sharing a Cpu share the same readerDepth slot,
 * which is what makes interrupt re-entrancy relevant.
 *)
cpu_of(t) == IF t \in Cpus THEN t ELSE CHOOSE c \in Cpus : TRUE

TypeOK ==
    /\ present \subseteq Keys
    /\ tomb \subseteq Keys
    /\ retired \subseteq Keys
    /\ fullyUnlinked \subseteq retired
    /\ present \cap tomb = {}
    /\ retiredAt \in [Keys -> 0..MaxEpoch]
    /\ globalEpoch \in 0..MaxEpoch
    /\ readerDepth \in [Cpus -> 0..MaxReadDepth]
    /\ readerEpoch \in [Cpus -> 0..MaxEpoch]
    /\ pc \in [Threads -> PcStates]
    /\ op \in [Threads -> Ops]
    /\ opKey \in [Threads -> Keys]
    /\ level \in [Threads -> 0..MaxLevels]
    /\ retryTick \in [Threads -> 0..MaxRetryTicks]
    /\ height \in [Keys -> 1..MaxLevels]
    /\ upMarked \in [Keys -> SUBSET (1..MaxLevels)]
    /\ interfered \in [Threads -> BOOLEAN]
    /\ finishParity \in {0, 1}
    /\ removeMarks \in [Threads -> 0..2]
    /\ Threads # {}
    /\ Cpus # {}

Init ==
    /\ present = {}
    /\ tomb = {}
    /\ retired = {}
    /\ fullyUnlinked = {}
    /\ retiredAt = [k \in Keys |-> 0]
    /\ globalEpoch = 1
    /\ readerDepth = [c \in Cpus |-> 0]
    /\ readerEpoch = [c \in Cpus |-> 0]
    /\ pc = [t \in Threads |-> "idle"]
    /\ op = [t \in Threads |-> "none"]
    /\ opKey = [t \in Threads |-> CHOOSE k \in Keys : TRUE]
    /\ level = [t \in Threads |-> 0]
    /\ retryTick = [t \in Threads |-> 0]
    /\ height = [k \in Keys |-> 1]
    /\ upMarked = [k \in Keys |-> {}]
    /\ interfered = [t \in Threads |-> FALSE]
    /\ finishParity = 0
    /\ removeMarks = [t \in Threads |-> 0]

InitRemoveScenario ==
    /\ LET key == CHOOSE k \in Keys : TRUE IN present = {key}
    /\ tomb = {}
    /\ retired = {}
    /\ fullyUnlinked = {}
    /\ retiredAt = [k \in Keys |-> 0]
    /\ globalEpoch = 1
    /\ readerDepth = [c \in Cpus |-> 0]
    /\ readerEpoch = [c \in Cpus |-> 0]
    /\ pc = [t \in Threads |-> "idle"]
    /\ op = [t \in Threads |-> "none"]
    /\ opKey = [t \in Threads |-> CHOOSE k \in Keys : TRUE]
    /\ level = [t \in Threads |-> 0]
    /\ retryTick = [t \in Threads |-> 0]
    /\ height = [k \in Keys |-> 1]
    /\ upMarked = [k \in Keys |-> {}]
    /\ interfered = [t \in Threads |-> FALSE]
    /\ finishParity = 0
    /\ removeMarks = [t \in Threads |-> 0]

TickRetry(t) ==
    [retryTick EXCEPT ![t] = IF retryTick[t] < MaxRetryTicks THEN retryTick[t] + 1 ELSE MaxRetryTicks]

ResetRetry(t) ==
    [retryTick EXCEPT ![t] = 0]

(***************************************************************************)
(* Per-CPU epoch read-section helpers.                                     *)
(*                                                                          *)
(* EnterRead models `enter_read_section`:                                  *)
(*   previous_depth = read_depth_.fetch_add(1)                             *)
(*   if (previous_depth == 0) { epoch_.store(global_epoch_) }              *)
(*                                                                          *)
(* Crucially, if a CPU is already inside a read section (readerDepth > 0)  *)
(* -- i.e. an interrupt fires while a thread is in a skiplist op -- the    *)
(* epoch snapshot is NOT updated.  The nested caller inherits the outer    *)
(* epoch, which is safe because the outer epoch was recorded first.        *)
(* This is the interrupt re-entrancy property modeled here.                *)
(***************************************************************************)
EnterRead(t) ==
    LET c == cpu_of(t) IN
    /\ readerDepth[c] < MaxReadDepth
    /\ readerDepth' = [readerDepth EXCEPT ![c] = @ + 1]
    /\ readerEpoch' = [readerEpoch EXCEPT ![c] =
                          IF readerDepth[c] = 0
                          THEN globalEpoch    \* first entry: snapshot current epoch
                          ELSE @]            \* nested entry: inherit outer epoch

ExitRead(t) ==
    LET c == cpu_of(t) IN
    /\ readerDepth[c] > 0
    /\ readerDepth' = [readerDepth EXCEPT ![c] = @ - 1]
    /\ readerEpoch' = readerEpoch

FinishOpCore(t) ==
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ op' = [op EXCEPT ![t] = "none"]
    /\ level' = [level EXCEPT ![t] = 0]
    /\ retryTick' = [retryTick EXCEPT ![t] = 0]
    /\ finishParity' = 1 - finishParity

FinishOp(t) ==
    /\ FinishOpCore(t)
    /\ ExitRead(t)

(***************************************************************************)
(* Epoch advancement predicate.                                            *)
(*                                                                          *)
(* `CanAdvanceEpoch(callerCpu)` matches C++ `try_advance_epoch`:           *)
(*   For every CPU slot i:                                                  *)
(*     - self-exempt if sole occupant (depth <= 1)                         *)
(*     - skip if idle (depth = 0)                                          *)
(*     - else require reader_epoch >= globalEpoch                          *)
(* Uses readerDepth as the authoritative reader-presence indicator,        *)
(* matching the fixed C++ which eliminated the racy reader_active_ flag.   *)
(***************************************************************************)
CanAdvanceEpoch(callerCpu) ==
    \A c \in Cpus :
        (c = callerCpu /\ readerDepth[c] <= 1)
        \/ (readerDepth[c] = 0)
        \/ (readerEpoch[c] >= globalEpoch)

(***************************************************************************)
(* Reclaim predicate.                                                      *)
(*                                                                          *)
(* `CanReclaim(callerCpu, epoch)` matches C++                              *)
(* `reclaim_from_per_cpu_list`:                                            *)
(*   reclaim_epoch = epoch - EpochLag                                      *)
(*   For every CPU slot i:                                                 *)
(*     - self-exempt ONLY if readerDepth <= 1 (sole occupant)              *)
(*     - skip if idle (depth = 0), matching reader_is_active()=depth>0    *)
(*     - else require reader_epoch > reclaim_epoch                         *)
(*   Uses readerDepth as authoritative reader-presence indicator,          *)
(*   matching the fixed C++ which eliminated the racy reader_active_ flag. *)
(***************************************************************************)
CanReclaim(callerCpu, epoch) ==
    LET reclaimEpoch == IF epoch > EpochLag THEN epoch - EpochLag ELSE 0 IN
    \A c \in Cpus :
        (c = callerCpu /\ readerDepth[c] <= 1)
        \/ (readerDepth[c] = 0)
        \/ (readerEpoch[c] > reclaimEpoch)

(***************************************************************************)
(* Reclaimable nodes: those that are:                                      *)
(*   - In fullyUnlinked (unlink_state::FULLY_UNLINKED set)                 *)
(*   - Retired epoch satisfies the lag condition                           *)
(* Matches C++ reclaim gate:                                               *)
(*   reclaim_eligible = (unlink_state == FULLY_UNLINKED)                  *)
(*                   && (retired_epoch_ <= reclaim_epoch)                  *)
(* where reclaim_epoch = globalEpoch - EpochLag                           *)
(***************************************************************************)
ReclaimableNodes(epoch) ==
    { k \in fullyUnlinked :
        /\ retiredAt[k] > 0
        /\ retiredAt[k] + EpochLag <= epoch }

(***************************************************************************)
(* AdvanceAndReclaim: combines try_advance_epoch + reclaim_retired_nodes.  *)
(* Called by remove() (throttled) and through the public advance_and_reclaim(). *)
(* `callerCpu` is exempted from both the advance and reclaim guards,      *)
(* matching the C++ `try_advance_epoch(slot)` and                         *)
(* `reclaim_from_per_cpu_list(slot)` self-exemption.                      *)
(***************************************************************************)
AdvanceAndReclaim(callerCpu) ==
    LET nextEpoch   == IF CanAdvanceEpoch(callerCpu)
                          THEN IF globalEpoch < MaxEpoch THEN globalEpoch + 1 ELSE MaxEpoch
                          ELSE globalEpoch IN
    LET reclaimable == IF CanReclaim(callerCpu, nextEpoch) THEN ReclaimableNodes(nextEpoch) ELSE {} IN
    /\ globalEpoch' = nextEpoch
    /\ retired' = retired \ reclaimable
    /\ fullyUnlinked' = fullyUnlinked \ reclaimable
    /\ retiredAt' = retiredAt   \* retiredAt entries for reclaimed keys now irrelevant

\* A CAS fails only because another thread changed the word after this thread read it.  Every step that changes
\*   a link gives the other threads a token; a failure consumes the failing thread's token.
Interfere(t) == interfered' = [u \in Threads |-> IF u = t THEN interfered[t] ELSE TRUE]
ConsumeInterference(t) == /\ interfered[t]
                          /\ interfered' = [interfered EXCEPT ![t] = FALSE]

(***************************************************************************)
(* START operations                                                        *)
(***************************************************************************)

StartOp(t, k, o, p) ==
    /\ pc[t] = "idle"
    /\ EnterRead(t)
    /\ pc' = [pc EXCEPT ![t] = p]
    /\ op' = [op EXCEPT ![t] = o]
    /\ opKey' = [opKey EXCEPT ![t] = k]
    /\ level' = [level EXCEPT ![t] = 0]
    /\ retryTick' = [retryTick EXCEPT ![t] = 0]
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch, finishParity>>
    /\ UNCHANGED links

StartInsert(t, k) == StartOp(t, k, "insert", "insert_find") /\ UNCHANGED removeMarks
StartRemove(t, k) == StartOp(t, k, "remove", "remove_find") /\ removeMarks' = [removeMarks EXCEPT ![t] = 0]
StartFind(t, k)   == StartOp(t, k, "find", "find_scan") /\ UNCHANGED removeMarks

(***************************************************************************)
(* advance_and_reclaim(slot): public, called outside a read section (and   *)
(* by remove(), throttled).  One step: try to advance the epoch, then      *)
(* reclaim what the lag allows.  (Replaces the former gc(), which no       *)
(* longer exists.)                                                         *)
(***************************************************************************)
ReclaimCall(t) ==
    /\ pc[t] = "idle"
    /\ AdvanceAndReclaim(cpu_of(t))
    /\ finishParity' = 1 - finishParity
    /\ UNCHANGED <<present, tomb, readerDepth, readerEpoch, pc, op, opKey, level, retryTick, removeMarks>>
    /\ UNCHANGED links

(***************************************************************************)
(* INSERT                                                                  *)
(*   RETRY_INSERT: find_with_nodelists -> FOUND: return false;             *)
(*   else CAS level 0 (bounded retries, then return false), then publish   *)
(*   the upper levels (bounded; a failure skips the level); return true.   *)
(***************************************************************************)
InsertFindReject(t) ==
    /\ pc[t] = "insert_find"
    /\ op[t] = "insert"
    /\ opKey[t] \in (present \cup tomb)
    /\ FinishOp(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch, opKey, removeMarks>>
    /\ UNCHANGED links

InsertFindMiss(t) ==
    /\ pc[t] = "insert_find"
    /\ op[t] = "insert"
    /\ opKey[t] \notin (present \cup tomb)
    /\ pc' = [pc EXCEPT ![t] = "insert_link0"]
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, op, opKey, level, retryTick, finishParity, removeMarks>>
    /\ UNCHANGED links

InsertLink0Success(t) ==
    /\ pc[t] = "insert_link0"
    /\ op[t] = "insert"
    /\ opKey[t] \notin (present \cup tomb)
    /\ present' = present \cup {opKey[t]}
    /\ \E h \in 1..MaxLevels : height' = [height EXCEPT ![opKey[t]] = h]
    /\ upMarked' = [upMarked EXCEPT ![opKey[t]] = {}]
    /\ Interfere(t)
    /\ pc' = [pc EXCEPT ![t] = "insert_link_upper"]
    /\ UNCHANGED <<tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, op, opKey, level, retryTick, finishParity, removeMarks>>

\* CAS failure on level 0: search again (bounded by MaxRetryTicks)
InsertLink0Fail(t) ==
    /\ pc[t] = "insert_link0"
    /\ op[t] = "insert"
    /\ ConsumeInterference(t)
    /\ retryTick[t] < MaxRetryTicks
    /\ pc' = [pc EXCEPT ![t] = "insert_find"]
    /\ retryTick' = TickRetry(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, op, opKey, level, finishParity, removeMarks>>
    /\ UNCHANGED <<height, upMarked>>

\* MAX_INSERT_LEVEL0_RETRIES: free the new node and return false
InsertLink0Abort(t) ==
    /\ pc[t] = "insert_link0"
    /\ op[t] = "insert"
    /\ retryTick[t] = MaxRetryTicks
    /\ FinishOp(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch, opKey, removeMarks>>
    /\ UNCHANGED links

\* Upper levels published (bounded retries per level; a failure skips the level); return true
InsertUpperFinish(t) ==
    /\ pc[t] = "insert_link_upper"
    /\ op[t] = "insert"
    /\ FinishOp(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch, opKey, removeMarks>>
    /\ IF height[opKey[t]] > 1 THEN Interfere(t) ELSE UNCHANGED interfered
    /\ UNCHANGED <<height, upMarked>>

(***************************************************************************)
(* REMOVE                                                                  *)
(*   RETRY_REMOVE: find_with_nodelists -> not FOUND: return false.         *)
(*   Mark the upper levels top-down (each bounded; exhausted: goto         *)
(*   RETRY_REMOVE).  Then the level-0 mark CAS:                            *)
(*     success      -> the node is deleted; unlink and retire it if its    *)
(*                     upper levels drained (else a traversal will);       *)
(*                     throttled advance_and_reclaim; return true          *)
(*     marked       -> another remove deleted it: return false             *)
(*     else (CAS lost to a successor change) -> retry, bounded             *)
(*                     (MAX_REMOVE_BOTTOM_RETRIES: return false); the      *)
(*                     re-walk may restart the call (stale handle, or the  *)
(*                     key now belongs to a re-inserted node).             *)
(***************************************************************************)
RemoveFindMissOrSoftDeleted(t) ==
    /\ pc[t] = "remove_find"
    /\ op[t] = "remove"
    /\ opKey[t] \notin present
    /\ FinishOp(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch, opKey, removeMarks>>
    /\ UNCHANGED links

\* Found: height[k] - 1 upper levels to mark, top-down
RemoveFindHit(t) ==
    /\ pc[t] = "remove_find"
    /\ op[t] = "remove"
    /\ opKey[t] \in present
    /\ level' = [level EXCEPT ![t] = height[opKey[t]] - 1]
    /\ pc' = [pc EXCEPT ![t] = IF height[opKey[t]] = 1 THEN "remove_bottom" ELSE "remove_mark"]
    /\ retryTick' = ResetRetry(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, op, opKey, finishParity, removeMarks>>
    /\ UNCHANGED links

\* Mark one upper level (soft_delete).  Deletes nothing: the key stays present.  A level that is already
\*   tombstoned is skipped without a write.
RemoveMarkProgress(t) ==
    /\ pc[t] = "remove_mark"
    /\ op[t] = "remove"
    /\ level[t] > 0
    /\ IF level[t] \in upMarked[opKey[t]]
          THEN UNCHANGED <<upMarked, interfered>>
          ELSE /\ upMarked' = [upMarked EXCEPT ![opKey[t]] = @ \cup {level[t]}]
               /\ Interfere(t)
    /\ UNCHANGED height
    /\ level' = [level EXCEPT ![t] = @ - 1]
    /\ pc' = [pc EXCEPT ![t] = IF level[t] = 1 THEN "remove_bottom" ELSE "remove_mark"]
    /\ retryTick' = ResetRetry(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, op, opKey, finishParity, removeMarks>>

\* soft_delete lost a race on this level: try the level again
RemoveMarkRetry(t) ==
    /\ pc[t] = "remove_mark"
    /\ op[t] = "remove"
    /\ ConsumeInterference(t)
    /\ retryTick[t] < MaxRetryTicks
    /\ retryTick' = TickRetry(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, pc, op, opKey, level, finishParity, removeMarks>>
    /\ UNCHANGED <<height, upMarked>>

\* MAX_REMOVE_MARK_RETRIES_PER_LEVEL: goto RETRY_REMOVE
RemoveMarkGotoRetry(t) ==
    /\ pc[t] = "remove_mark"
    /\ op[t] = "remove"
    /\ retryTick[t] = MaxRetryTicks
    /\ pc' = [pc EXCEPT ![t] = "remove_find"]
    /\ retryTick' = ResetRetry(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, op, opKey, level, finishParity, removeMarks>>
    /\ UNCHANGED links

\* Level-0 mark CAS succeeds: the node is deleted (present -> tomb).  If its upper levels have drained, this
\*   call unlinks and retires it; either way it may run the throttled advance_and_reclaim, then returns true.
RemoveBottomMark(t) ==
    /\ pc[t] = "remove_bottom"
    /\ op[t] = "remove"
    /\ opKey[t] \in present
    /\ LET k == opKey[t] IN
       LET c == cpu_of(t) IN
       \E unlinkNow \in BOOLEAN, reclaimNow \in BOOLEAN :
          LET newRetired == IF unlinkNow THEN retired \cup {k} ELSE retired IN
          LET newFU      == IF unlinkNow THEN fullyUnlinked \cup {k} ELSE fullyUnlinked IN
          LET newRetAt   == IF unlinkNow THEN [retiredAt EXCEPT ![k] = globalEpoch] ELSE retiredAt IN
          LET nextEpoch  == IF reclaimNow /\ CanAdvanceEpoch(c)
                              THEN IF globalEpoch < MaxEpoch THEN globalEpoch + 1 ELSE MaxEpoch
                              ELSE globalEpoch IN
          LET reclaimable == IF reclaimNow /\ CanReclaim(c, nextEpoch)
                               THEN { x \in newFU : newRetAt[x] > 0 /\ newRetAt[x] + EpochLag <= nextEpoch }
                               ELSE {} IN
          /\ present' = present \ {k}
          /\ tomb' = IF unlinkNow THEN tomb ELSE tomb \cup {k}
          /\ retired' = newRetired \ reclaimable
          /\ fullyUnlinked' = newFU \ reclaimable
          /\ retiredAt' = newRetAt
          /\ globalEpoch' = nextEpoch
    /\ removeMarks' = [removeMarks EXCEPT ![t] = @ + 1]
    /\ FinishOp(t)
    /\ UNCHANGED opKey
    /\ Interfere(t)
    /\ UNCHANGED <<height, upMarked>>

\* The level-0 link is already marked: another remove() deleted the node.  Return false.
RemoveBottomStolenByOther(t) ==
    /\ pc[t] = "remove_bottom"
    /\ op[t] = "remove"
    /\ opKey[t] \notin present
    /\ FinishOp(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch, opKey, removeMarks>>
    /\ UNCHANGED links

\* The mark CAS lost to a change of the node's successor: retry (the re-walk finds the same node)
RemoveBottomRetryContinue(t) ==
    /\ pc[t] = "remove_bottom"
    /\ op[t] = "remove"
    /\ ConsumeInterference(t)
    /\ opKey[t] \in present
    /\ retryTick[t] < MaxRetryTicks
    /\ retryTick' = TickRetry(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, pc, op, opKey, level, finishParity, removeMarks>>
    /\ UNCHANGED <<height, upMarked>>

\* The re-walk after a lost CAS restarts the call: a stale handle, or the key now belongs to a re-inserted node
\*   whose upper levels this call has not marked (goto RETRY_REMOVE).
RemoveBottomRewalkRestart(t) ==
    /\ pc[t] = "remove_bottom"
    /\ op[t] = "remove"
    /\ ConsumeInterference(t)
    /\ retryTick[t] < MaxRetryTicks
    /\ pc' = [pc EXCEPT ![t] = "remove_find"]
    /\ retryTick' = ResetRetry(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch,
                   readerDepth, readerEpoch, op, opKey, level, finishParity, removeMarks>>
    /\ UNCHANGED <<height, upMarked>>

\* MAX_REMOVE_BOTTOM_RETRIES (or MAX_REMOVE_LOCAL_ASSIST_FAILURES): return false
RemoveBottomGiveUp(t) ==
    /\ pc[t] = "remove_bottom"
    /\ op[t] = "remove"
    /\ retryTick[t] = MaxRetryTicks
    /\ FinishOp(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch, opKey, removeMarks>>
    /\ UNCHANGED links

(***************************************************************************)
(* FIND: a read-section-wrapped traversal.  It may meet a tombstoned node  *)
(* whose upper levels have drained and unlink and retire it (the helper    *)
(* path every traversal - find, insert, remove - takes).                   *)
(***************************************************************************)
FindScanNoAssist(t) ==
    /\ pc[t] = "find_scan"
    /\ op[t] = "find"
    /\ FinishOp(t)
    /\ UNCHANGED <<present, tomb, retired, fullyUnlinked, retiredAt, globalEpoch, opKey, removeMarks>>
    /\ UNCHANGED links

FindScanAssist(t) ==
    /\ pc[t] = "find_scan"
    /\ op[t] = "find"
    /\ \E k \in tomb :
          /\ tomb' = tomb \ {k}
          /\ retired' = retired \cup {k}
          /\ fullyUnlinked' = fullyUnlinked \cup {k}
          /\ retiredAt' = [retiredAt EXCEPT ![k] = globalEpoch]
    /\ UNCHANGED <<present, globalEpoch, opKey, removeMarks>>
    /\ FinishOp(t)
    /\ Interfere(t)
    /\ UNCHANGED <<height, upMarked>>

(***************************************************************************)
(* Per-thread step relation                                                *)
(***************************************************************************)
RemoveThreadStep(t) ==
    \/ \E k \in Keys : StartRemove(t, k)
    \/ RemoveFindMissOrSoftDeleted(t)
    \/ RemoveFindHit(t)
    \/ RemoveMarkProgress(t)
    \/ RemoveMarkRetry(t)
    \/ RemoveMarkGotoRetry(t)
    \/ RemoveBottomMark(t)
    \/ RemoveBottomStolenByOther(t)
    \/ RemoveBottomRetryContinue(t)
    \/ RemoveBottomRewalkRestart(t)
    \/ RemoveBottomGiveUp(t)

ThreadStep(t) ==
    \/ \E k \in Keys : StartInsert(t, k)
    \/ \E k \in Keys : StartFind(t, k)
    \/ ReclaimCall(t)
    \/ InsertFindReject(t)
    \/ InsertFindMiss(t)
    \/ InsertLink0Success(t)
    \/ InsertLink0Fail(t)
    \/ InsertLink0Abort(t)
    \/ InsertUpperFinish(t)
    \/ RemoveThreadStep(t)
    \/ FindScanNoAssist(t)
    \/ FindScanAssist(t)

Next == \E t \in Threads : ThreadStep(t)

Spec == Init /\ [][Next]_vars

SpecFair ==
    Spec /\ (\A t \in Threads : WF_vars(ThreadStep(t)))

SpecRemoveFair ==
    InitRemoveScenario
    /\ [][\E t \in Threads : RemoveThreadStep(t)]_vars
    /\ (\A t \in Threads : WF_vars(RemoveThreadStep(t)))

(***************************************************************************)
(* Safety invariants                                                       *)
(***************************************************************************)

\* present and tomb are always disjoint
StateConsistency == present \cap tomb = {}

\* NOTE: RetiredDisjointFromPresent is intentionally NOT listed here.
\* In the C++ implementation `retired` tracks old *node instances*; a key can
\* be retired (old node) while simultaneously being re-inserted as a new node
\* (new allocation). The model abstracts nodes to keys, so retired \cap present
\* can be non-empty when a key is recycled -- this is correct behaviour.

\* fully-unlinked is always a subset of retired
FullyUnlinkedSubsetRetired == fullyUnlinked \subseteq retired

\* NOTE: TombDisjointFromRetired is intentionally NOT checked as a safety invariant.
\* In C++ retired and tomb are per-node-instance sets. Key re-use means a new node for
\* key k can be tagged for removal (tomb) while an old node for the same k sits in
\* the retired list. At the key-abstraction level used in this model, tomb \cap retired
\* can therefore be non-empty -- this is correct behaviour.
TombDisjointFromRetired == tomb \cap retired = {}

\* Interrupt re-entrancy invariant:
\* Any CPU inside a read section (depth >= 1) must have a non-zero epoch
\* snapshot (i.e., outermost entry recorded the global epoch correctly).
InterruptReentrancySafe ==
    \A c \in Cpus :
        (readerDepth[c] >= 1) => (readerEpoch[c] > 0)

\* One remove() call deletes at most one node (it returns right after its level-0 mark)
RemoveDeletesAtMostOne == \A t \in Threads : removeMarks[t] <= 1

(***************************************************************************)
(* Liveness properties                                                     *)
(***************************************************************************)

EventuallyIdle == []<>(\A t \in Threads : pc[t] = "idle")

\* Every operation finishes (wait-freedom).  Holds for one thread; with more, a lock-free remove() may restart
\*   indefinitely while other threads keep completing operations, which lock-freedom allows.
ActiveOpsEventuallyFinish ==
    \A t \in Threads : []((pc[t] # "idle") => <>(pc[t] = "idle"))

\* Lock-freedom: while operations are in progress, some operation keeps completing.
GlobalProgress ==
    \/ <>[](\A t \in Threads : pc[t] = "idle")
    \/ ([]<>(finishParity = 0) /\ []<>(finishParity = 1))

RemoveBottomEventuallyIdle ==
    \A t \in Threads : []((pc[t] = "remove_bottom") => <>(pc[t] = "idle"))

RemoveOpsEventuallyFinish ==
    \A t \in Threads : []((op[t] = "remove" /\ pc[t] # "idle") => <>(pc[t] = "idle"))

RemoveNoMarkLivelock ==
    ~<>([](\E t \in Threads : pc[t] = "remove_mark"))

EpochBounded == [] (globalEpoch \in 0..MaxEpoch)

=============================================================================
