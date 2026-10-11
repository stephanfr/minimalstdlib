--------------------------- MODULE LockfreeSingleArena ---------------------------
(***************************************************************************)
(* TLA+ / PlusCal model of minstd::pmr::lockfree_single_arena_resource      *)
(* (include/__memory_resource/lockfree_single_arena_resource.h).            *)
(*                                                                         *)
(* One label = one shared-memory access in the C++ code (an atomic load,   *)
(* store, CAS or exchange), so TLC explores every interleaving the         *)
(* hardware can produce at that granularity.  Each label is annotated with *)
(* the C++ function it models.                                             *)
(*                                                                         *)
(* What is modelled                                                        *)
(*   - The frontier (next_empty_memory_block_) and frontier allocation      *)
(*     (get_next_empty_memory_block), including the back-link that lets    *)
(*     the frontier roll back.                                             *)
(*   - Block headers (metadata_index_, size_including_header_,             *)
(*     previous_block_) at every arena unit, so stale or misplaced header  *)
(*     reads/writes are visible.                                           *)
(*   - Metadata records: packed block_state_ (pointer + state), total_size_,*)
(*     original_shard_; the 8 allocation_state values that are reachable. *)
(*   - Metadata block managers: reserve/release of active slots, per-       *)
(*     manager free lists, the bump path (current_metadata_record_count_)  *)
(*     with its collision check against the frontier, and                  *)
(*     trim_metadata_records.                                              *)
(*   - Size-class free-block bins per shard, search_for_deallocated_block   *)
(*     with its CLAIMED / RETRY (re-push) / RECLAIM_DEFERRED / DISCARD     *)
(*     outcomes.                                                           *)
(*   - Per-shard pending-deallocation lists and counts, maintenance windows *)
(*     (maybe_open_maintenance_window, run_maintenance_window,             *)
(*     run_maintenance_window_all), reclaim_pending_frontier_batch,        *)
(*     process_pending_batch and try_reclaim_frontier_blocks.              *)
(*   - Interrupts: one handler process per CPU that may run allocate or    *)
(*     deallocate whenever its CPU is outside an interrupt_guard, while    *)
(*     the interrupted code on that CPU makes no progress.                 *)
(*                                                                         *)
(* Fix switches (see the remediation plan)                                 *)
(*   FrontierMode      "pre_cas_backlink"  previous_block_ of next written  *)
(*                                         BEFORE the frontier CAS         *)
(*                                         (origin/More-fixes)             *)
(*                     "post_cas_hint"     written after the CAS, reclaimer *)
(*                                         validates the hint (N3)         *)
(*                     "packed"            predecessor packed into the     *)
(*                                         frontier word, each block keeps *)
(*                                         its own back-link (N4)          *)
(*   SerializeReclaim  TRUE: frontier_reclaim_in_progress_ try-flag (N4)    *)
(*   PendingCountFirst TRUE: count_ incremented before the push (N5)       *)
(*   GuardTrim         TRUE: trim_metadata_records holds an interrupt_guard *)
(*                     while a manager is TRIMMING (N6)                    *)
(*   FixedBoundary     TRUE: after its CAS, each side re-checks the other   *)
(*                     (behind a seq_cst fence in C++; TLA+ is SC) and,    *)
(*                     on overlap, undoes its claim once it is on top of   *)
(*                     its own stack, or keeps it if the overlap has gone  *)
(*                     (grow_metadata_region, get_next_empty_memory_block) *)
(*                                                                         *)
(* Abstractions (each documented in README.md)                             *)
(*   - Arena memory is counted in 64-byte units; a block is [start,start+n).*)
(*     Metadata record i occupies unit MS - i, as in the C++ layout.       *)
(*   - intrusive_tagged_stack push/pop are single atomic steps on a set    *)
(*     (the tag counter makes the C++ CAS ABA-safe; M15 covers the link    *)
(*     race).  Pop order is nondeterministic, which over-approximates LIFO.*)
(*   - Address bins are collapsed into one list per (size class, shard).   *)
(*   - Packed-state versions are not modelled: every allocation is freed   *)
(*     exactly once by the thread that holds it (the API contract), so the *)
(*     version never decides a CAS.                                        *)
(*   - reclaim_pending_frontier_batch's scan of the batch, and a window's  *)
(*     batching of the stolen list, are single steps.                      *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    NumCPUs,            \* CPUs = shards; one thread per CPU
    UseInterrupts,      \* TRUE: one interrupt-handler process per CPU
    ArenaUnits,         \* arena size in 64-byte units (blocks + metadata)
    RecordsPerManager,  \* METADATA_RECORDS_PER_BLOCK (4096 in C++)
    NumManagers,        \* metadata_block_manager_capacity_
    Sizes,              \* block sizes in units, incl. header (each = one size bin)
    Threshold,          \* MAINTENANCE_WINDOW_THRESHOLD
    OpsPerThread,       \* allocate/deallocate calls per thread, e.g. <<2, 2>>
    MaxIrqOps,          \* allocate/deallocate calls per interrupt handler
    FrontierMode,       \* "pre_cas_backlink" | "post_cas_hint" | "packed"
    SerializeReclaim,   \* N4 reclaim flag
    PendingCountFirst,  \* N5 count-before-push
    GuardTrim,          \* N6 interrupt_guard around trim's TRIMMING section
    FixedBoundary,      \* frontier/metadata boundary: re-check after each CAS, undo from the top (item 1)
    InitialBacklinks    \* initial previous_block_ contents: the arena buffer is not zeroed

ASSUME FrontierMode \in {"pre_cas_backlink", "post_cas_hint", "packed"}
ASSUME SerializeReclaim \in BOOLEAN /\ PendingCountFirst \in BOOLEAN /\ UseInterrupts \in BOOLEAN /\ GuardTrim \in BOOLEAN
ASSUME FixedBoundary \in BOOLEAN

NULL == -1
CPUs == 1..NumCPUs
MainProcs == CPUs
IrqProcs == IF UseInterrupts THEN {c + NumCPUs : c \in CPUs} ELSE {}
AllProcs == MainProcs \cup IrqProcs
CpuOf(p) == IF p \in MainProcs THEN p ELSE p - NumCPUs
MaxMeta == RecordsPerManager * NumManagers
Metas == 0..(MaxMeta - 1)
Managers == 0..(NumManagers - 1)
Units == 0..(ArenaUnits - 1)
MS == ArenaUnits - 1                      \* metadata_start_: unit of record 0
ManagerOf(m) == m \div RecordsPerManager
ShardOrder(s) == [i \in 1..NumCPUs |-> ((s - 1 + i - 1) % NumCPUs) + 1]
Min(S) == CHOOSE x \in S : \A y \in S : x <= y
NoBlock == [start |-> NULL, size |-> 0]

\* Values for OpsPerThread (assigned with <- in a .cfg; .cfg files cannot hold tuples)
Ops_2_2 == <<2, 2>>
Ops_1_3 == <<1, 3>>
Ops_1_1 == <<1, 1>>
Ops_2_1 == <<2, 1>>
Ops_3 == <<3>>
Ops_2_2_2 == <<2, 2, 2>>
Ops_1_4 == <<1, 4>>
Ops_1_1_1_1_1 == <<1, 1, 1, 1, 1>>

\* Values for InitialBacklinks (assigned with <- in a .cfg)
CleanArena == [u \in Units |-> NULL]
\* Unit 5 still holds a back-link to unit 2, as if [2,5) had once been the block before a frontier at 5.
StaleBacklinkAt5 == [u \in Units |-> IF u = 5 THEN 2 ELSE NULL]

(* --algorithm LockfreeSingleArena {
variables
    \* next_empty_memory_block_: tag modes use (ptr, counter); "packed" uses (ptr, predecessor)
    fwPtr = 0, fwCtr = 0, fwPred = NULL,

    \* block_header fields at every unit (only meaningful at a block start)
    hdrMeta = [u \in Units |-> NULL],
    hdrSize = [u \in Units |-> 0],
    hdrPrev = InitialBacklinks,

    \* block_metadata
    mState = [m \in Metas |-> "INVALID"],
    mPtr   = [m \in Metas |-> NULL],
    mSize  = [m \in Metas |-> 0],
    mShard = [m \in Metas |-> 1],
    metaCount = 0,                                   \* current_metadata_record_count_

    \* metadata_block_manager: state_and_active_count_ and local_free_head_
    mgrActive   = [k \in Managers |-> 0],
    mgrTrimming = [k \in Managers |-> FALSE],
    mgrFree     = [k \in Managers |-> {}],

    bins = [z \in Sizes |-> [c \in CPUs |-> {}]],    \* free_block_bins_
    pending = [c \in CPUs |-> <<>>],                  \* pending_shards_[c].head_ (LIFO: head first)
    pendingCount = [c \in CPUs |-> 0],                \* pending_shards_[c].count_
    windowOpen = [c \in CPUs |-> FALSE],              \* maintenance_window_open_
    reclaimBusy = FALSE,                              \* frontier_reclaim_in_progress_ (N4)

    guardDepth = [c \in CPUs |-> 0],                  \* interrupt_guard nesting per CPU
    irqActive = [c \in CPUs |-> FALSE],

    \* procedure results
    rvMeta = [p \in AllProcs |-> NULL],
    rvIdx  = [p \in AllProcs |-> NULL],
    rvBlk  = [p \in AllProcs |-> NULL],

    \* ghost state
    live = {},                                        \* allocations owned by callers
    held = [p \in AllProcs |-> {}],                   \* ... per caller
    stolenOutstanding = [c \in CPUs |-> 0];           \* stolen, count_ not yet reduced

define {
    InUserRegion(u) == \E a \in live : a.start < u /\ u < a.start + a.size
    InBins(m)    == \E z \in Sizes, c \in CPUs : m \in bins[z][c]
    PendSet(c)   == {pending[c][i] : i \in 1..Len(pending[c])}
    InPending(m) == \E c \in CPUs : m \in PendSet(c)
    InMgrFree(m) == \E k \in Managers : m \in mgrFree[k]
    InAnyList(m) == InBins(m) \/ InPending(m) \/ InMgrFree(m)
}

macro guard_enter() { guardDepth[CpuOf(self)] := guardDepth[CpuOf(self)] + 1; }
macro guard_exit()  { guardDepth[CpuOf(self)] := guardDepth[CpuOf(self)] - 1; }

\* ------------------------------------------------------------------ recycle_metadata
procedure RecycleMetadata(rm)
{
 RM_state:
    mState[rm] := "METADATA_AVAILABLE";                         \* block_state_.store(pack(nullptr, METADATA_AVAILABLE))
    mPtr[rm] := NULL;
 RM_push:
    assert ~InAnyList(rm);                                      \* guarded free_header_stack::push
    mgrFree[ManagerOf(rm)] := mgrFree[ManagerOf(rm)] \cup {rm};
 RM_release:
    mgrActive[ManagerOf(rm)] := mgrActive[ManagerOf(rm)] - 1;   \* manager.release_active_slot()
    return;
}

\* ------------------------------------------------------------------ try_reclaim_frontier_blocks
procedure TryReclaim()
variables tPtr = 0, tCtr = 0, tPred = NULL, tPrev = NULL, tMi = NULL, tPP = NULL;
{
 TR_flag:
    if (SerializeReclaim) {                                     \* N4: only one thread rolls the frontier back
        if (reclaimBusy) { return; } else { reclaimBusy := TRUE; };
    };
 TR_load:
    tPtr := fwPtr; tCtr := fwCtr; tPred := fwPred;              \* next_empty_memory_block_.load
    if (FrontierMode = "packed") {
        tPrev := fwPred;                                        \* predecessor comes from the same word
        if (fwPred = NULL) { goto TR_done; } else { goto TR_meta; };
    };
 TR_hint:
    assert ~InUserRegion(tPtr);
    tPrev := hdrPrev[tPtr];                                     \* frontier_ptr->previous_block_
    if (hdrPrev[tPtr] = NULL \/ (FrontierMode = "post_cas_hint" /\ ~(hdrPrev[tPtr] < tPtr))) {
        goto TR_done;                                           \* sentinel, or N3 range check failed
    };
 TR_meta:
    assert ~InUserRegion(tPrev);                                \* reclaimer must not read user data
    tMi := hdrMeta[tPrev];                                      \* prev->metadata_index_
 TR_count:
    if (tMi = NULL \/ tMi >= metaCount) { goto TR_done; };
 TR_state:
    if (mPtr[tMi] # tPrev \/ mState[tMi] # "AVAILABLE") { goto TR_done; };   \* meta->block_state_.load
 TR_adj:
    if (FrontierMode = "post_cas_hint") {                       \* N3 adjacency check (absent with the packed frontier)
        assert ~InUserRegion(tPrev);
        if (tPrev + hdrSize[tPrev] # tPtr) { goto TR_done; };
    };
 TR_claim:
    if (mState[tMi] = "AVAILABLE" /\ mPtr[tMi] = tPrev) {     \* CAS AVAILABLE -> FRONTIER_RECLAIM_IN_PROGRESS
        mState[tMi] := "FRONTIER_RECLAIM_IN_PROGRESS";
    } else { goto TR_done; };
 TR_pp:
    if (FrontierMode = "packed") { tPP := hdrPrev[tPrev]; };    \* N4: prev's own back-link, final once claimed
 TR_cas:
    if (fwPtr = tPtr /\ fwCtr = tCtr /\ fwPred = tPred) {      \* frontier CAS
        fwPtr := tPrev;
        fwCtr := IF FrontierMode = "packed" THEN fwCtr ELSE fwCtr + 1;
        fwPred := IF FrontierMode = "packed" THEN tPP ELSE NULL;
    } else { goto TR_restore; };
 TR_reclaimed:
    assert mState[tMi] = "FRONTIER_RECLAIM_IN_PROGRESS";
    mState[tMi] := "FRONTIER_RECLAIMED";
    goto TR_load;
 TR_restore:
    if (mState[tMi] = "FRONTIER_RECLAIM_IN_PROGRESS") { mState[tMi] := "AVAILABLE"; };
 TR_done:
    if (SerializeReclaim) { reclaimBusy := FALSE; };
    return;
}

\* ------------------------------------------------------------------ trim_metadata_records
procedure Trim()
variables xc = 0, xk = 0, xshrunk = FALSE;
{
 TM_load:
    if (metaCount = 0) { return; }
    else { xc := metaCount; xk := (metaCount - 1) \div RecordsPerManager; };
 TM_claim:
    if (mgrActive[xk] = 0 /\ ~mgrTrimming[xk]) {
        mgrTrimming[xk] := TRUE;
        if (GuardTrim) { guard_enter(); };                      \* N6: no same-CPU interrupt while TRIMMING
    } else { return; };
 TM_recheck:
    if (metaCount # xc) { goto TM_release; };
 TM_shrink:
    if (metaCount = xc) { metaCount := xk * RecordsPerManager; xshrunk := TRUE; } else { xshrunk := FALSE; };
 TM_reset:
    if (xshrunk) { mgrFree[xk] := {}; };
 TM_release:
    mgrTrimming[xk] := FALSE;
    if (GuardTrim) { guard_exit(); };
    goto TM_load;
}

\* ------------------------------------------------------------------ run_maintenance_window
procedure RunWindow(rs)
variables stolen = <<>>, batch = {}, nstolen = 0, cand = NULL, bsPtr = 0, bsCtr = 0, bsPred = NULL, bpp = NULL, pm = NULL;
{
 RW_steal:
    stolen := pending[rs];                                      \* steal_pending_deallocation_list (guarded exchange)
    nstolen := Len(pending[rs]);
    stolenOutstanding[rs] := stolenOutstanding[rs] + Len(pending[rs]);
    pending[rs] := <<>>;
    if (stolen = <<>>) { goto RW_tail; };
 \* Walk the stolen list head-first, handing MAINTENANCE_WINDOW_BATCH_SIZE (= Threshold) entries at a time
 \*   to process_pending_batch.
 RW_chunk:
    while (stolen # <<>>) {
        batch := {stolen[i] : i \in 1..IF Len(stolen) < Threshold THEN Len(stolen) ELSE Threshold};
        stolen := SubSeq(stolen, (IF Len(stolen) < Threshold THEN Len(stolen) ELSE Threshold) + 1, Len(stolen));
 \* ---- reclaim_pending_frontier_batch
 RW_bflag:
        if (SerializeReclaim) {
            if (reclaimBusy) { goto RW_publish; } else { reclaimBusy := TRUE; };
        };
 RW_bload:
        bsPtr := fwPtr; bsCtr := fwCtr; bsPred := fwPred;
 RW_bfind:
        with (C = {m \in batch : mState[m] = "DEALLOCATED_PENDING" /\ mPtr[m] + mSize[m] = bsPtr}) {
            if (C = {}) { goto RW_bdone; } else { cand := Min(C); };
        };
 RW_bclaim:
        if (mState[cand] = "DEALLOCATED_PENDING") { mState[cand] := "FRONTIER_RECLAIM_IN_PROGRESS"; }
        else { goto RW_bload; };
 RW_bpp:
        if (FrontierMode = "packed") { bpp := hdrPrev[mPtr[cand]]; };
 RW_bcas:
        if (fwPtr = bsPtr /\ fwCtr = bsCtr /\ fwPred = bsPred) {
            fwPtr := mPtr[cand];
            fwCtr := IF FrontierMode = "packed" THEN fwCtr ELSE fwCtr + 1;
            fwPred := IF FrontierMode = "packed" THEN bpp ELSE NULL;
        } else { goto RW_brestore; };
 RW_brecycle:
        batch := batch \ {cand};
        call RecycleMetadata(cand);
        goto RW_bload;
 RW_brestore:
        if (mState[cand] = "FRONTIER_RECLAIM_IN_PROGRESS") { mState[cand] := "DEALLOCATED_PENDING"; };
        goto RW_bload;
 RW_bdone:
        if (SerializeReclaim) { reclaimBusy := FALSE; };
 \* ---- process_pending_batch: publish the rest of this batch to the free bins
 RW_publish:
        while (batch # {}) {
            pm := Min(batch);
            batch := batch \ {Min(batch)};
 RW_pub_state:
            if (mState[pm] = "DEALLOCATED_PENDING") { mState[pm] := "AVAILABLE"; } else { goto RW_publish; };
 RW_pub_push:
            assert ~InAnyList(pm);
            bins[mSize[pm]][mShard[pm]] := bins[mSize[pm]][mShard[pm]] \cup {pm};
        };
    };
 RW_reduce:                                                     \* reduce_pending_deallocation_count
    assert PendingCountFirst => pendingCount[rs] >= nstolen;    \* N5's MINIMAL_STD_ASSERT
    pendingCount[rs] := IF pendingCount[rs] > nstolen THEN pendingCount[rs] - nstolen ELSE 0;
    stolenOutstanding[rs] := stolenOutstanding[rs] - nstolen;
    goto RW_steal;
 RW_tail:
    call TryReclaim();
 RW_trim:
    call Trim();
 RW_ret:
    return;
}

\* ------------------------------------------------------------------ maybe_open_maintenance_window
procedure MaybeWindow(ws, wforce)
{
 MW_count:
    if (pendingCount[ws] = 0 \/ (~wforce /\ pendingCount[ws] < Threshold)) { return; };
 MW_open:
    if (windowOpen[ws]) { return; } else { windowOpen[ws] := TRUE; };
 MW_run:
    call RunWindow(ws);
 MW_close:
    windowOpen[ws] := FALSE;
    return;
}

\* ------------------------------------------------------------------ run_maintenance_window_all (no window flag)
procedure RunWindowAll()
variables wa = 1;
{
 WA_loop:
    while (wa <= NumCPUs) {
        call RunWindow(wa);
 WA_next:
        wa := wa + 1;
    };
 WA_ret:
    return;
}

\* ------------------------------------------------------------------ search_for_deallocated_block
procedure Search(qsz, qshard)
variables qi = 1, qpop = NULL, qdef = NULL, qclaimed = NULL;
{
 SR_restart:
    guard_enter();
    qi := 1; qdef := NULL; qclaimed := NULL;
 SR_scan:
    while (qi <= NumCPUs) {
 SR_pop:                                                        \* free_block_stack::pop
        with (sh = ShardOrder(qshard)[qi]) {
            if (bins[qsz][sh] = {}) { qpop := NULL; }
            else { with (x \in bins[qsz][sh]) { qpop := x; bins[qsz][sh] := bins[qsz][sh] \ {x}; }; };
        };
        if (qpop = NULL) { qi := qi + 1; goto SR_scan; };
 SR_claim:                                                      \* unguarded_try_claim_popped_block
        if (mState[qpop] = "AVAILABLE") { mState[qpop] := "LOCKED"; qclaimed := qpop; goto SR_exit; }
        else if (mState[qpop] = "FRONTIER_RECLAIM_IN_PROGRESS") { goto SR_repush; }
        else if (mState[qpop] = "FRONTIER_RECLAIMED") { qdef := qpop; goto SR_exit; }
        else { qi := qi + 1; goto SR_scan; };                   \* DISCARD
 SR_repush:                                                     \* RETRY: re-push and keep scanning
        assert ~InAnyList(qpop);
        with (sh = ShardOrder(qshard)[qi]) { bins[qsz][sh] := bins[qsz][sh] \cup {qpop}; };
        qi := qi + 1;
    };
 SR_exit:
    guard_exit();
    if (qdef # NULL) { call RecycleMetadata(qdef); goto SR_restart; }
    else if (qclaimed # NULL) { rvMeta[self] := qclaimed; return; }
    else { rvMeta[self] := NULL; return; };
}

\* ------------------------------------------------------------------ get_next_metadata_record_index
procedure GetMetaIndex()
variables gc = 0, gk = 0, gtail = 0;
{
 GM_load:
    gc := metaCount;
    if (metaCount = 0) { goto GM_bump; }
    else { gtail := (metaCount - 1) \div RecordsPerManager; gk := 0; };
 GM_scan:
    while (gk <= gtail) {
 GM_reserve:                                                    \* manager.reserve_active_slot()
        if (mgrTrimming[gk]) { gk := gk + 1; goto GM_scan; }
        else { mgrActive[gk] := mgrActive[gk] + 1; };
 GM_pop:                                                        \* guarded free_header_stack::pop
        if (mgrFree[gk] # {}) {
            with (x \in mgrFree[gk]) { mgrFree[gk] := mgrFree[gk] \ {x}; rvIdx[self] := x; };
            return;
        };
 GM_release:
        mgrActive[gk] := mgrActive[gk] - 1;
        gk := gk + 1;
    };
 GM_bump:
    if (gc \div RecordsPerManager >= NumManagers) { rvIdx[self] := NULL; return; }
    else { gk := gc \div RecordsPerManager; };
 GM_breserve:
    \* reserve_active_slot() spins while the manager is TRIMMING, re-reading the count each time;
    \*   modelled as blocking until either can change.
    await ~mgrTrimming[gk] \/ metaCount # gc;
    if (mgrTrimming[gk]) { gc := metaCount; goto GM_bump; }
    else { mgrActive[gk] := mgrActive[gk] + 1; };
 GM_bcheck:                                                     \* new metadata must not reach the frontier
    if (MS - (gc + 1) <= fwPtr) { goto GM_bfail; };
 GM_bcas:
    if (metaCount = gc) {
        metaCount := gc + 1;
        if (~FixedBoundary) { rvIdx[self] := gc; return; };
    } else { gc := metaCount; goto GM_brelease; };
 GM_brecheck:                                                   \* grow_metadata_region: fence, then re-check the frontier
    if (MS - (gc + 1) > fwPtr) { rvIdx[self] := gc; return; };  \* GROWN
 GM_bundo:                                                      \* overlap: give the record back, but only from the top
    if (metaCount = gc + 1) { metaCount := gc; goto GM_bfail; } \* NO_ROOM
    else { goto GM_brecheck; };                                 \* back off: a record above ours is undoing too
 GM_brelease:
    mgrActive[gk] := mgrActive[gk] - 1;
    goto GM_bump;
 GM_bfail:
    mgrActive[gk] := mgrActive[gk] - 1;
    rvIdx[self] := NULL;
    return;
}

\* ------------------------------------------------------------------ get_next_empty_memory_block
procedure GetEmptyBlock(gsz)
variables sPtr = 0, sCtr = 0, sPred = NULL, nxt = 0;
{
 FB_load:
    guard_enter();
    sPtr := fwPtr; sCtr := fwCtr; sPred := fwPred;
 FB_check:
    if (sPtr + gsz >= MS - (metaCount + 1)) {                   \* would intrude into metadata
        guard_exit();
        rvBlk[self] := NULL;
        return;
    } else { nxt := sPtr + gsz; };
 FB_pre:
    if (FrontierMode = "pre_cas_backlink") {                    \* next->previous_block_ = current BEFORE the CAS
        assert ~InUserRegion(nxt);
        hdrPrev[nxt] := sPtr;
    };
 FB_cas:
    if (fwPtr = sPtr /\ fwCtr = sCtr /\ fwPred = sPred) {
        fwPtr := nxt;
        fwCtr := IF FrontierMode = "packed" THEN fwCtr ELSE fwCtr + 1;
        fwPred := IF FrontierMode = "packed" THEN sPtr ELSE NULL;
        if (FrontierMode = "pre_cas_backlink") { guard_exit(); };   \* original: size written after the guard
        if (~FixedBoundary) { goto FB_size; };
    } else {
        sPtr := fwPtr; sCtr := fwCtr; sPred := fwPred;           \* CAS failure returns the current value
        goto FB_check;
    };
 FB_recheck:                                                    \* fence, then re-check the metadata count
    if (nxt < MS - (metaCount + 1)) { goto FB_size; };
 FB_undo:                                                       \* overlap: roll the frontier back while our block is on top
    if (fwPtr = nxt /\ fwPred = sPtr /\ fwCtr = sCtr) {
        fwPtr := sPtr; fwPred := sPred;
        guard_exit();
        rvBlk[self] := NULL;
        return;
    } else if (nxt < MS - (metaCount + 1)) { goto FB_size; }    \* a trim removed the overlap: keep the block
    else { goto FB_undo; };                                     \* back off: a block above ours is undoing too
 FB_size:
    assert ~InUserRegion(sPtr);
    hdrSize[sPtr] := gsz;                                       \* current->size_including_header_
 FB_link:
    if (FrontierMode = "post_cas_hint") {                       \* N3: next->previous_block_ after the CAS
        assert ~InUserRegion(nxt);
        hdrPrev[nxt] := sPtr;
    } else if (FrontierMode = "packed") {                       \* N4: current->previous_block_ = predecessor
        hdrPrev[sPtr] := sPred;
    };
    if (FrontierMode # "pre_cas_backlink") { guard_exit(); };
    rvBlk[self] := sPtr;
    return;
}

\* ------------------------------------------------------------------ do_allocate
procedure Allocate(asz)
variables ameta = NULL, ablk = NULL, areserved = NULL;
{
 AL_window:
    call MaybeWindow(CpuOf(self), FALSE);
 AL_search:
    call Search(asz, CpuOf(self));
 AL_found:
    if (rvMeta[self] # NULL) { ameta := rvMeta[self]; ablk := mPtr[rvMeta[self]]; goto AL_fin; }
    else { call GetMetaIndex(); };
 AL_reserved:
    if (rvIdx[self] = NULL) { goto AL_out; }
    else { areserved := rvIdx[self]; call GetEmptyBlock(asz); };
 AL_frontier:
    if (rvBlk[self] # NULL) { ablk := rvBlk[self]; ameta := areserved; goto AL_fin; }
    else { call MaybeWindow(CpuOf(self), TRUE); };
 AL_search2:
    call Search(asz, CpuOf(self));
 AL_found2:
    if (rvMeta[self] # NULL) { goto AL_reuse; }
    else { call RunWindowAll(); };
 AL_search3:
    call Search(asz, CpuOf(self));
 AL_found3:
    if (rvMeta[self] # NULL) { goto AL_reuse; }
    else { call RecycleMetadata(areserved); goto AL_out; };     \* out of memory: return the reserved record
 AL_reuse:
    ameta := rvMeta[self]; ablk := mPtr[rvMeta[self]];
    call RecycleMetadata(areserved);                            \* reuse won: return the reserved record
 AL_fin:
    assert ~InUserRegion(ablk);
    assert hdrSize[ablk] = asz;
    hdrMeta[ablk] := ameta;                                     \* free_block->metadata_index_
 AL_inuse:
    mState[ameta] := "IN_USE";                                  \* block_state_.store(pack(free_block, IN_USE))
    mPtr[ameta] := ablk;
 AL_fields:
    mSize[ameta] := hdrSize[ablk];                              \* total_size_, original_shard_
    mShard[ameta] := CpuOf(self);
 AL_live:
    live := live \cup {[start |-> ablk, size |-> asz]};
    held[self] := held[self] \cup {[start |-> ablk, size |-> asz]};
 AL_out:
    return;
}

\* ------------------------------------------------------------------ do_try_deallocate
procedure Deallocate(da)
variables dmi = NULL, dtarget = 1;
{
 DA_user:
    live := live \ {da};                                        \* caller hands the block back
    guard_enter();
 DA_idx:
    dmi := hdrMeta[da.start];                                   \* header.metadata_index_
 DA_count_check:
    assert dmi # NULL /\ dmi < metaCount;                       \* a valid free must never be rejected
 DA_lock:
    assert mState[dmi] = "IN_USE" /\ mPtr[dmi] = da.start;      \* load + CAS IN_USE -> LOCKED
    mState[dmi] := "LOCKED";
    guard_exit();
 DA_pending:
    mState[dmi] := "DEALLOCATED_PENDING";
    dtarget := mShard[dmi];
    if (PendingCountFirst) { goto DA_count1; };
 DA_push0:                                                      \* original: push, then count
    assert ~InAnyList(dmi);
    pending[dtarget] := <<dmi>> \o pending[dtarget];
 DA_count0:
    pendingCount[dtarget] := pendingCount[dtarget] + 1;
    goto DA_window;
 DA_count1:                                                     \* N5: count, then push (one guard)
    guard_enter();
    pendingCount[dtarget] := pendingCount[dtarget] + 1;
 DA_push1:
    assert ~InAnyList(dmi);
    pending[dtarget] := <<dmi>> \o pending[dtarget];
    guard_exit();
 DA_window:
    call MaybeWindow(dtarget, FALSE);
 DA_ret:
    return;
}

\* ------------------------------------------------------------------ threads
process (Main \in MainProcs)
variables ops = 0, asel = 0, dsel = NoBlock;
{
 MN_loop:
    while (ops < OpsPerThread[self]) {
        ops := ops + 1;
        either {
            with (z \in Sizes) { asel := z; };
            call Allocate(asel);
        } or {
            await held[self] # {};
            with (a \in held[self]) { dsel := a; held[self] := held[self] \ {a}; };
            call Deallocate(dsel);
        };
    };
}

process (Irq \in IrqProcs)
variables iops = 0, iasel = 0, idsel = NoBlock;
{
 IQ_loop:
    while (iops < MaxIrqOps) {
        await guardDepth[CpuOf(self)] = 0;                      \* interrupts enabled on this CPU
        irqActive[CpuOf(self)] := TRUE;
        iops := iops + 1;
        either {
            with (z \in Sizes) { iasel := z; };
            call Allocate(iasel);
        } or {
            await held[self] # {};
            with (a \in held[self]) { idsel := a; held[self] := held[self] \ {a}; };
            call Deallocate(idsel);
        };
 IQ_end:
        irqActive[CpuOf(self)] := FALSE;
    };
}
} *)

\* BEGIN TRANSLATION
CONSTANT defaultInitValue
VARIABLES pc, fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, hdrPrev, mState, mPtr, 
          mSize, mShard, metaCount, mgrActive, mgrTrimming, mgrFree, bins, 
          pending, pendingCount, windowOpen, reclaimBusy, guardDepth, 
          irqActive, rvMeta, rvIdx, rvBlk, live, held, stolenOutstanding, 
          stack

(* define statement *)
InUserRegion(u) == \E a \in live : a.start < u /\ u < a.start + a.size
InBins(m)    == \E z \in Sizes, c \in CPUs : m \in bins[z][c]
PendSet(c)   == {pending[c][i] : i \in 1..Len(pending[c])}
InPending(m) == \E c \in CPUs : m \in PendSet(c)
InMgrFree(m) == \E k \in Managers : m \in mgrFree[k]
InAnyList(m) == InBins(m) \/ InPending(m) \/ InMgrFree(m)

VARIABLES rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, stolen, 
          batch, nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
          qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, sPtr, 
          sCtr, sPred, nxt, asz, ameta, ablk, areserved, da, dmi, dtarget, 
          ops, asel, dsel, iops, iasel, idsel

vars == << pc, fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, hdrPrev, mState, mPtr, 
           mSize, mShard, metaCount, mgrActive, mgrTrimming, mgrFree, bins, 
           pending, pendingCount, windowOpen, reclaimBusy, guardDepth, 
           irqActive, rvMeta, rvIdx, rvBlk, live, held, stolenOutstanding, 
           stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
           stolen, batch, nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
           wforce, wa, qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, gtail, 
           gsz, sPtr, sCtr, sPred, nxt, asz, ameta, ablk, areserved, da, dmi, 
           dtarget, ops, asel, dsel, iops, iasel, idsel >>

ProcSet == (MainProcs) \cup (IrqProcs)

Init == (* Global variables *)
        /\ fwPtr = 0
        /\ fwCtr = 0
        /\ fwPred = NULL
        /\ hdrMeta = [u \in Units |-> NULL]
        /\ hdrSize = [u \in Units |-> 0]
        /\ hdrPrev = InitialBacklinks
        /\ mState = [m \in Metas |-> "INVALID"]
        /\ mPtr = [m \in Metas |-> NULL]
        /\ mSize = [m \in Metas |-> 0]
        /\ mShard = [m \in Metas |-> 1]
        /\ metaCount = 0
        /\ mgrActive = [k \in Managers |-> 0]
        /\ mgrTrimming = [k \in Managers |-> FALSE]
        /\ mgrFree = [k \in Managers |-> {}]
        /\ bins = [z \in Sizes |-> [c \in CPUs |-> {}]]
        /\ pending = [c \in CPUs |-> <<>>]
        /\ pendingCount = [c \in CPUs |-> 0]
        /\ windowOpen = [c \in CPUs |-> FALSE]
        /\ reclaimBusy = FALSE
        /\ guardDepth = [c \in CPUs |-> 0]
        /\ irqActive = [c \in CPUs |-> FALSE]
        /\ rvMeta = [p \in AllProcs |-> NULL]
        /\ rvIdx = [p \in AllProcs |-> NULL]
        /\ rvBlk = [p \in AllProcs |-> NULL]
        /\ live = {}
        /\ held = [p \in AllProcs |-> {}]
        /\ stolenOutstanding = [c \in CPUs |-> 0]
        (* Procedure RecycleMetadata *)
        /\ rm = [ self \in ProcSet |-> defaultInitValue]
        (* Procedure TryReclaim *)
        /\ tPtr = [ self \in ProcSet |-> 0]
        /\ tCtr = [ self \in ProcSet |-> 0]
        /\ tPred = [ self \in ProcSet |-> NULL]
        /\ tPrev = [ self \in ProcSet |-> NULL]
        /\ tMi = [ self \in ProcSet |-> NULL]
        /\ tPP = [ self \in ProcSet |-> NULL]
        (* Procedure Trim *)
        /\ xc = [ self \in ProcSet |-> 0]
        /\ xk = [ self \in ProcSet |-> 0]
        /\ xshrunk = [ self \in ProcSet |-> FALSE]
        (* Procedure RunWindow *)
        /\ rs = [ self \in ProcSet |-> defaultInitValue]
        /\ stolen = [ self \in ProcSet |-> <<>>]
        /\ batch = [ self \in ProcSet |-> {}]
        /\ nstolen = [ self \in ProcSet |-> 0]
        /\ cand = [ self \in ProcSet |-> NULL]
        /\ bsPtr = [ self \in ProcSet |-> 0]
        /\ bsCtr = [ self \in ProcSet |-> 0]
        /\ bsPred = [ self \in ProcSet |-> NULL]
        /\ bpp = [ self \in ProcSet |-> NULL]
        /\ pm = [ self \in ProcSet |-> NULL]
        (* Procedure MaybeWindow *)
        /\ ws = [ self \in ProcSet |-> defaultInitValue]
        /\ wforce = [ self \in ProcSet |-> defaultInitValue]
        (* Procedure RunWindowAll *)
        /\ wa = [ self \in ProcSet |-> 1]
        (* Procedure Search *)
        /\ qsz = [ self \in ProcSet |-> defaultInitValue]
        /\ qshard = [ self \in ProcSet |-> defaultInitValue]
        /\ qi = [ self \in ProcSet |-> 1]
        /\ qpop = [ self \in ProcSet |-> NULL]
        /\ qdef = [ self \in ProcSet |-> NULL]
        /\ qclaimed = [ self \in ProcSet |-> NULL]
        (* Procedure GetMetaIndex *)
        /\ gc = [ self \in ProcSet |-> 0]
        /\ gk = [ self \in ProcSet |-> 0]
        /\ gtail = [ self \in ProcSet |-> 0]
        (* Procedure GetEmptyBlock *)
        /\ gsz = [ self \in ProcSet |-> defaultInitValue]
        /\ sPtr = [ self \in ProcSet |-> 0]
        /\ sCtr = [ self \in ProcSet |-> 0]
        /\ sPred = [ self \in ProcSet |-> NULL]
        /\ nxt = [ self \in ProcSet |-> 0]
        (* Procedure Allocate *)
        /\ asz = [ self \in ProcSet |-> defaultInitValue]
        /\ ameta = [ self \in ProcSet |-> NULL]
        /\ ablk = [ self \in ProcSet |-> NULL]
        /\ areserved = [ self \in ProcSet |-> NULL]
        (* Procedure Deallocate *)
        /\ da = [ self \in ProcSet |-> defaultInitValue]
        /\ dmi = [ self \in ProcSet |-> NULL]
        /\ dtarget = [ self \in ProcSet |-> 1]
        (* Process Main *)
        /\ ops = [self \in MainProcs |-> 0]
        /\ asel = [self \in MainProcs |-> 0]
        /\ dsel = [self \in MainProcs |-> NoBlock]
        (* Process Irq *)
        /\ iops = [self \in IrqProcs |-> 0]
        /\ iasel = [self \in IrqProcs |-> 0]
        /\ idsel = [self \in IrqProcs |-> NoBlock]
        /\ stack = [self \in ProcSet |-> << >>]
        /\ pc = [self \in ProcSet |-> CASE self \in MainProcs -> "MN_loop"
                                        [] self \in IrqProcs -> "IQ_loop"]

RM_state(self) == /\ pc[self] = "RM_state"
                  /\ mState' = [mState EXCEPT ![rm[self]] = "METADATA_AVAILABLE"]
                  /\ mPtr' = [mPtr EXCEPT ![rm[self]] = NULL]
                  /\ pc' = [pc EXCEPT ![self] = "RM_push"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mSize, mShard, metaCount, mgrActive, 
                                  mgrTrimming, mgrFree, bins, pending, 
                                  pendingCount, windowOpen, reclaimBusy, 
                                  guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                  live, held, stolenOutstanding, stack, rm, 
                                  tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                  xshrunk, rs, stolen, batch, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                  wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                  gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                  asz, ameta, ablk, areserved, da, dmi, 
                                  dtarget, ops, asel, dsel, iops, iasel, idsel >>

RM_push(self) == /\ pc[self] = "RM_push"
                 /\ Assert(~InAnyList(rm[self]), 
                           "Failure of assertion at line 181, column 5.")
                 /\ mgrFree' = [mgrFree EXCEPT ![ManagerOf(rm[self])] = mgrFree[ManagerOf(rm[self])] \cup {rm[self]}]
                 /\ pc' = [pc EXCEPT ![self] = "RM_release"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, bins, 
                                 pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                 xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                 cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                 wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                 qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                 sPred, nxt, asz, ameta, ablk, areserved, da, 
                                 dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                 idsel >>

RM_release(self) == /\ pc[self] = "RM_release"
                    /\ mgrActive' = [mgrActive EXCEPT ![ManagerOf(rm[self])] = mgrActive[ManagerOf(rm[self])] - 1]
                    /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                    /\ rm' = [rm EXCEPT ![self] = Head(stack[self]).rm]
                    /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrTrimming, mgrFree, bins, 
                                    pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, tPtr, tCtr, tPred, 
                                    tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                    stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                    bsPred, bpp, pm, ws, wforce, wa, qsz, 
                                    qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                    gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                    ameta, ablk, areserved, da, dmi, dtarget, 
                                    ops, asel, dsel, iops, iasel, idsel >>

RecycleMetadata(self) == RM_state(self) \/ RM_push(self)
                            \/ RM_release(self)

TR_flag(self) == /\ pc[self] = "TR_flag"
                 /\ IF SerializeReclaim
                       THEN /\ IF reclaimBusy
                                  THEN /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                       /\ tPtr' = [tPtr EXCEPT ![self] = Head(stack[self]).tPtr]
                                       /\ tCtr' = [tCtr EXCEPT ![self] = Head(stack[self]).tCtr]
                                       /\ tPred' = [tPred EXCEPT ![self] = Head(stack[self]).tPred]
                                       /\ tPrev' = [tPrev EXCEPT ![self] = Head(stack[self]).tPrev]
                                       /\ tMi' = [tMi EXCEPT ![self] = Head(stack[self]).tMi]
                                       /\ tPP' = [tPP EXCEPT ![self] = Head(stack[self]).tPP]
                                       /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                                       /\ UNCHANGED reclaimBusy
                                  ELSE /\ reclaimBusy' = TRUE
                                       /\ pc' = [pc EXCEPT ![self] = "TR_load"]
                                       /\ UNCHANGED << stack, tPtr, tCtr, 
                                                       tPred, tPrev, tMi, tPP >>
                       ELSE /\ pc' = [pc EXCEPT ![self] = "TR_load"]
                            /\ UNCHANGED << reclaimBusy, stack, tPtr, tCtr, 
                                            tPred, tPrev, tMi, tPP >>
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                 live, held, stolenOutstanding, rm, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

TR_load(self) == /\ pc[self] = "TR_load"
                 /\ tPtr' = [tPtr EXCEPT ![self] = fwPtr]
                 /\ tCtr' = [tCtr EXCEPT ![self] = fwCtr]
                 /\ tPred' = [tPred EXCEPT ![self] = fwPred]
                 /\ IF FrontierMode = "packed"
                       THEN /\ tPrev' = [tPrev EXCEPT ![self] = fwPred]
                            /\ IF fwPred = NULL
                                  THEN /\ pc' = [pc EXCEPT ![self] = "TR_done"]
                                  ELSE /\ pc' = [pc EXCEPT ![self] = "TR_meta"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "TR_hint"]
                            /\ tPrev' = tPrev
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tMi, tPP, xc, xk, xshrunk, rs, 
                                 stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                 bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                 qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                 sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                 areserved, da, dmi, dtarget, ops, asel, dsel, 
                                 iops, iasel, idsel >>

TR_hint(self) == /\ pc[self] = "TR_hint"
                 /\ Assert(~InUserRegion(tPtr[self]), 
                           "Failure of assertion at line 203, column 5.")
                 /\ tPrev' = [tPrev EXCEPT ![self] = hdrPrev[tPtr[self]]]
                 /\ IF hdrPrev[tPtr[self]] = NULL \/ (FrontierMode = "post_cas_hint" /\ ~(hdrPrev[tPtr[self]] < tPtr[self]))
                       THEN /\ pc' = [pc EXCEPT ![self] = "TR_done"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "TR_meta"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tPtr, tCtr, tPred, tMi, tPP, xc, 
                                 xk, xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

TR_meta(self) == /\ pc[self] = "TR_meta"
                 /\ Assert(~InUserRegion(tPrev[self]), 
                           "Failure of assertion at line 209, column 5.")
                 /\ tMi' = [tMi EXCEPT ![self] = hdrMeta[tPrev[self]]]
                 /\ pc' = [pc EXCEPT ![self] = "TR_count"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tPtr, tCtr, tPred, tPrev, tPP, xc, 
                                 xk, xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

TR_count(self) == /\ pc[self] = "TR_count"
                  /\ IF tMi[self] = NULL \/ tMi[self] >= metaCount
                        THEN /\ pc' = [pc EXCEPT ![self] = "TR_done"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "TR_state"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                  nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, 
                                  ws, wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                  sPred, nxt, asz, ameta, ablk, areserved, da, 
                                  dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                  idsel >>

TR_state(self) == /\ pc[self] = "TR_state"
                  /\ IF mPtr[tMi[self]] # tPrev[self] \/ mState[tMi[self]] # "AVAILABLE"
                        THEN /\ pc' = [pc EXCEPT ![self] = "TR_done"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "TR_adj"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                  nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, 
                                  ws, wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                  sPred, nxt, asz, ameta, ablk, areserved, da, 
                                  dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                  idsel >>

TR_adj(self) == /\ pc[self] = "TR_adj"
                /\ IF FrontierMode = "post_cas_hint"
                      THEN /\ Assert(~InUserRegion(tPrev[self]), 
                                     "Failure of assertion at line 217, column 9.")
                           /\ IF tPrev[self] + hdrSize[tPrev[self]] # tPtr[self]
                                 THEN /\ pc' = [pc EXCEPT ![self] = "TR_done"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "TR_claim"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "TR_claim"]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                sPred, nxt, asz, ameta, ablk, areserved, da, 
                                dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                idsel >>

TR_claim(self) == /\ pc[self] = "TR_claim"
                  /\ IF mState[tMi[self]] = "AVAILABLE" /\ mPtr[tMi[self]] = tPrev[self]
                        THEN /\ mState' = [mState EXCEPT ![tMi[self]] = "FRONTIER_RECLAIM_IN_PROGRESS"]
                             /\ pc' = [pc EXCEPT ![self] = "TR_pp"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "TR_done"]
                             /\ UNCHANGED mState
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mPtr, mSize, mShard, metaCount, 
                                  mgrActive, mgrTrimming, mgrFree, bins, 
                                  pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                  nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, 
                                  ws, wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                  sPred, nxt, asz, ameta, ablk, areserved, da, 
                                  dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                  idsel >>

TR_pp(self) == /\ pc[self] = "TR_pp"
               /\ IF FrontierMode = "packed"
                     THEN /\ tPP' = [tPP EXCEPT ![self] = hdrPrev[tPrev[self]]]
                     ELSE /\ TRUE
                          /\ tPP' = tPP
               /\ pc' = [pc EXCEPT ![self] = "TR_cas"]
               /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, hdrPrev, 
                               mState, mPtr, mSize, mShard, metaCount, 
                               mgrActive, mgrTrimming, mgrFree, bins, pending, 
                               pendingCount, windowOpen, reclaimBusy, 
                               guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                               live, held, stolenOutstanding, stack, rm, tPtr, 
                               tCtr, tPred, tPrev, tMi, xc, xk, xshrunk, rs, 
                               stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                               bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                               qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                               sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                               areserved, da, dmi, dtarget, ops, asel, dsel, 
                               iops, iasel, idsel >>

TR_cas(self) == /\ pc[self] = "TR_cas"
                /\ IF fwPtr = tPtr[self] /\ fwCtr = tCtr[self] /\ fwPred = tPred[self]
                      THEN /\ fwPtr' = tPrev[self]
                           /\ fwCtr' = (IF FrontierMode = "packed" THEN fwCtr ELSE fwCtr + 1)
                           /\ fwPred' = (IF FrontierMode = "packed" THEN tPP[self] ELSE NULL)
                           /\ pc' = [pc EXCEPT ![self] = "TR_reclaimed"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "TR_restore"]
                           /\ UNCHANGED << fwPtr, fwCtr, fwPred >>
                /\ UNCHANGED << hdrMeta, hdrSize, hdrPrev, mState, mPtr, mSize, 
                                mShard, metaCount, mgrActive, mgrTrimming, 
                                mgrFree, bins, pending, pendingCount, 
                                windowOpen, reclaimBusy, guardDepth, irqActive, 
                                rvMeta, rvIdx, rvBlk, live, held, 
                                stolenOutstanding, stack, rm, tPtr, tCtr, 
                                tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                areserved, da, dmi, dtarget, ops, asel, dsel, 
                                iops, iasel, idsel >>

TR_reclaimed(self) == /\ pc[self] = "TR_reclaimed"
                      /\ Assert(mState[tMi[self]] = "FRONTIER_RECLAIM_IN_PROGRESS", 
                                "Failure of assertion at line 233, column 5.")
                      /\ mState' = [mState EXCEPT ![tMi[self]] = "FRONTIER_RECLAIMED"]
                      /\ pc' = [pc EXCEPT ![self] = "TR_load"]
                      /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                      hdrPrev, mPtr, mSize, mShard, metaCount, 
                                      mgrActive, mgrTrimming, mgrFree, bins, 
                                      pending, pendingCount, windowOpen, 
                                      reclaimBusy, guardDepth, irqActive, 
                                      rvMeta, rvIdx, rvBlk, live, held, 
                                      stolenOutstanding, stack, rm, tPtr, tCtr, 
                                      tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                      rs, stolen, batch, nstolen, cand, bsPtr, 
                                      bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                      qsz, qshard, qi, qpop, qdef, qclaimed, 
                                      gc, gk, gtail, gsz, sPtr, sCtr, sPred, 
                                      nxt, asz, ameta, ablk, areserved, da, 
                                      dmi, dtarget, ops, asel, dsel, iops, 
                                      iasel, idsel >>

TR_restore(self) == /\ pc[self] = "TR_restore"
                    /\ IF mState[tMi[self]] = "FRONTIER_RECLAIM_IN_PROGRESS"
                          THEN /\ mState' = [mState EXCEPT ![tMi[self]] = "AVAILABLE"]
                          ELSE /\ TRUE
                               /\ UNCHANGED mState
                    /\ pc' = [pc EXCEPT ![self] = "TR_done"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mPtr, mSize, mShard, metaCount, 
                                    mgrActive, mgrTrimming, mgrFree, bins, 
                                    pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, stack, rm, tPtr, tCtr, 
                                    tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                    rs, stolen, batch, nstolen, cand, bsPtr, 
                                    bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                    qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                    gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                    asz, ameta, ablk, areserved, da, dmi, 
                                    dtarget, ops, asel, dsel, iops, iasel, 
                                    idsel >>

TR_done(self) == /\ pc[self] = "TR_done"
                 /\ IF SerializeReclaim
                       THEN /\ reclaimBusy' = FALSE
                       ELSE /\ TRUE
                            /\ UNCHANGED reclaimBusy
                 /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                 /\ tPtr' = [tPtr EXCEPT ![self] = Head(stack[self]).tPtr]
                 /\ tCtr' = [tCtr EXCEPT ![self] = Head(stack[self]).tCtr]
                 /\ tPred' = [tPred EXCEPT ![self] = Head(stack[self]).tPred]
                 /\ tPrev' = [tPrev EXCEPT ![self] = Head(stack[self]).tPrev]
                 /\ tMi' = [tMi EXCEPT ![self] = Head(stack[self]).tMi]
                 /\ tPP' = [tPP EXCEPT ![self] = Head(stack[self]).tPP]
                 /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                 live, held, stolenOutstanding, rm, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

TryReclaim(self) == TR_flag(self) \/ TR_load(self) \/ TR_hint(self)
                       \/ TR_meta(self) \/ TR_count(self) \/ TR_state(self)
                       \/ TR_adj(self) \/ TR_claim(self) \/ TR_pp(self)
                       \/ TR_cas(self) \/ TR_reclaimed(self)
                       \/ TR_restore(self) \/ TR_done(self)

TM_load(self) == /\ pc[self] = "TM_load"
                 /\ IF metaCount = 0
                       THEN /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                            /\ xc' = [xc EXCEPT ![self] = Head(stack[self]).xc]
                            /\ xk' = [xk EXCEPT ![self] = Head(stack[self]).xk]
                            /\ xshrunk' = [xshrunk EXCEPT ![self] = Head(stack[self]).xshrunk]
                            /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                       ELSE /\ xc' = [xc EXCEPT ![self] = metaCount]
                            /\ xk' = [xk EXCEPT ![self] = (metaCount - 1) \div RecordsPerManager]
                            /\ pc' = [pc EXCEPT ![self] = "TM_claim"]
                            /\ UNCHANGED << stack, xshrunk >>
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, rs, 
                                 stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                 bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                 qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                 sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                 areserved, da, dmi, dtarget, ops, asel, dsel, 
                                 iops, iasel, idsel >>

TM_claim(self) == /\ pc[self] = "TM_claim"
                  /\ IF mgrActive[xk[self]] = 0 /\ ~mgrTrimming[xk[self]]
                        THEN /\ mgrTrimming' = [mgrTrimming EXCEPT ![xk[self]] = TRUE]
                             /\ IF GuardTrim
                                   THEN /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] + 1]
                                   ELSE /\ TRUE
                                        /\ UNCHANGED guardDepth
                             /\ pc' = [pc EXCEPT ![self] = "TM_recheck"]
                             /\ UNCHANGED << stack, xc, xk, xshrunk >>
                        ELSE /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                             /\ xc' = [xc EXCEPT ![self] = Head(stack[self]).xc]
                             /\ xk' = [xk EXCEPT ![self] = Head(stack[self]).xk]
                             /\ xshrunk' = [xshrunk EXCEPT ![self] = Head(stack[self]).xshrunk]
                             /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                             /\ UNCHANGED << mgrTrimming, guardDepth >>
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrFree, bins, pending, 
                                  pendingCount, windowOpen, reclaimBusy, 
                                  irqActive, rvMeta, rvIdx, rvBlk, live, held, 
                                  stolenOutstanding, rm, tPtr, tCtr, tPred, 
                                  tPrev, tMi, tPP, rs, stolen, batch, nstolen, 
                                  cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                  wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                  sPred, nxt, asz, ameta, ablk, areserved, da, 
                                  dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                  idsel >>

TM_recheck(self) == /\ pc[self] = "TM_recheck"
                    /\ IF metaCount # xc[self]
                          THEN /\ pc' = [pc EXCEPT ![self] = "TM_release"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "TM_shrink"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrActive, mgrTrimming, mgrFree, 
                                    bins, pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, stack, rm, tPtr, tCtr, 
                                    tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                    rs, stolen, batch, nstolen, cand, bsPtr, 
                                    bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                    qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                    gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                    asz, ameta, ablk, areserved, da, dmi, 
                                    dtarget, ops, asel, dsel, iops, iasel, 
                                    idsel >>

TM_shrink(self) == /\ pc[self] = "TM_shrink"
                   /\ IF metaCount = xc[self]
                         THEN /\ metaCount' = xk[self] * RecordsPerManager
                              /\ xshrunk' = [xshrunk EXCEPT ![self] = TRUE]
                         ELSE /\ xshrunk' = [xshrunk EXCEPT ![self] = FALSE]
                              /\ UNCHANGED metaCount
                   /\ pc' = [pc EXCEPT ![self] = "TM_reset"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   mgrActive, mgrTrimming, mgrFree, bins, 
                                   pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                   tPP, xc, xk, rs, stolen, batch, nstolen, 
                                   cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                   wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                   qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                   sPred, nxt, asz, ameta, ablk, areserved, da, 
                                   dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                   idsel >>

TM_reset(self) == /\ pc[self] = "TM_reset"
                  /\ IF xshrunk[self]
                        THEN /\ mgrFree' = [mgrFree EXCEPT ![xk[self]] = {}]
                        ELSE /\ TRUE
                             /\ UNCHANGED mgrFree
                  /\ pc' = [pc EXCEPT ![self] = "TM_release"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, bins, 
                                  pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                  nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, 
                                  ws, wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                  sPred, nxt, asz, ameta, ablk, areserved, da, 
                                  dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                  idsel >>

TM_release(self) == /\ pc[self] = "TM_release"
                    /\ mgrTrimming' = [mgrTrimming EXCEPT ![xk[self]] = FALSE]
                    /\ IF GuardTrim
                          THEN /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] - 1]
                          ELSE /\ TRUE
                               /\ UNCHANGED guardDepth
                    /\ pc' = [pc EXCEPT ![self] = "TM_load"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrActive, mgrFree, bins, 
                                    pending, pendingCount, windowOpen, 
                                    reclaimBusy, irqActive, rvMeta, rvIdx, 
                                    rvBlk, live, held, stolenOutstanding, 
                                    stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                    tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                    nstolen, cand, bsPtr, bsCtr, bsPred, bpp, 
                                    pm, ws, wforce, wa, qsz, qshard, qi, qpop, 
                                    qdef, qclaimed, gc, gk, gtail, gsz, sPtr, 
                                    sCtr, sPred, nxt, asz, ameta, ablk, 
                                    areserved, da, dmi, dtarget, ops, asel, 
                                    dsel, iops, iasel, idsel >>

Trim(self) == TM_load(self) \/ TM_claim(self) \/ TM_recheck(self)
                 \/ TM_shrink(self) \/ TM_reset(self) \/ TM_release(self)

RW_steal(self) == /\ pc[self] = "RW_steal"
                  /\ stolen' = [stolen EXCEPT ![self] = pending[rs[self]]]
                  /\ nstolen' = [nstolen EXCEPT ![self] = Len(pending[rs[self]])]
                  /\ stolenOutstanding' = [stolenOutstanding EXCEPT ![rs[self]] = stolenOutstanding[rs[self]] + Len(pending[rs[self]])]
                  /\ pending' = [pending EXCEPT ![rs[self]] = <<>>]
                  /\ IF stolen'[self] = <<>>
                        THEN /\ pc' = [pc EXCEPT ![self] = "RW_tail"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "RW_chunk"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pendingCount, windowOpen, reclaimBusy, 
                                  guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                  live, held, stack, rm, tPtr, tCtr, tPred, 
                                  tPrev, tMi, tPP, xc, xk, xshrunk, rs, batch, 
                                  cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                  wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                  sPred, nxt, asz, ameta, ablk, areserved, da, 
                                  dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                  idsel >>

RW_chunk(self) == /\ pc[self] = "RW_chunk"
                  /\ IF stolen[self] # <<>>
                        THEN /\ batch' = [batch EXCEPT ![self] = {stolen[self][i] : i \in 1..IF Len(stolen[self]) < Threshold THEN Len(stolen[self]) ELSE Threshold}]
                             /\ stolen' = [stolen EXCEPT ![self] = SubSeq(stolen[self], (IF Len(stolen[self]) < Threshold THEN Len(stolen[self]) ELSE Threshold) + 1, Len(stolen[self]))]
                             /\ pc' = [pc EXCEPT ![self] = "RW_bflag"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "RW_reduce"]
                             /\ UNCHANGED << stolen, batch >>
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                  wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                  gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                  asz, ameta, ablk, areserved, da, dmi, 
                                  dtarget, ops, asel, dsel, iops, iasel, idsel >>

RW_bflag(self) == /\ pc[self] = "RW_bflag"
                  /\ IF SerializeReclaim
                        THEN /\ IF reclaimBusy
                                   THEN /\ pc' = [pc EXCEPT ![self] = "RW_publish"]
                                        /\ UNCHANGED reclaimBusy
                                   ELSE /\ reclaimBusy' = TRUE
                                        /\ pc' = [pc EXCEPT ![self] = "RW_bload"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "RW_bload"]
                             /\ UNCHANGED reclaimBusy
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                  live, held, stolenOutstanding, stack, rm, 
                                  tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                  xshrunk, rs, stolen, batch, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                  wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                  gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                  asz, ameta, ablk, areserved, da, dmi, 
                                  dtarget, ops, asel, dsel, iops, iasel, idsel >>

RW_bload(self) == /\ pc[self] = "RW_bload"
                  /\ bsPtr' = [bsPtr EXCEPT ![self] = fwPtr]
                  /\ bsCtr' = [bsCtr EXCEPT ![self] = fwCtr]
                  /\ bsPred' = [bsPred EXCEPT ![self] = fwPred]
                  /\ pc' = [pc EXCEPT ![self] = "RW_bfind"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                  nstolen, cand, bpp, pm, ws, wforce, wa, qsz, 
                                  qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                  gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                  ameta, ablk, areserved, da, dmi, dtarget, 
                                  ops, asel, dsel, iops, iasel, idsel >>

RW_bfind(self) == /\ pc[self] = "RW_bfind"
                  /\ LET C == {m \in batch[self] : mState[m] = "DEALLOCATED_PENDING" /\ mPtr[m] + mSize[m] = bsPtr[self]} IN
                       IF C = {}
                          THEN /\ pc' = [pc EXCEPT ![self] = "RW_bdone"]
                               /\ cand' = cand
                          ELSE /\ cand' = [cand EXCEPT ![self] = Min(C)]
                               /\ pc' = [pc EXCEPT ![self] = "RW_bclaim"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                  nstolen, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                  wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                  sPred, nxt, asz, ameta, ablk, areserved, da, 
                                  dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                  idsel >>

RW_bclaim(self) == /\ pc[self] = "RW_bclaim"
                   /\ IF mState[cand[self]] = "DEALLOCATED_PENDING"
                         THEN /\ mState' = [mState EXCEPT ![cand[self]] = "FRONTIER_RECLAIM_IN_PROGRESS"]
                              /\ pc' = [pc EXCEPT ![self] = "RW_bpp"]
                         ELSE /\ pc' = [pc EXCEPT ![self] = "RW_bload"]
                              /\ UNCHANGED mState
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mPtr, mSize, mShard, metaCount, 
                                   mgrActive, mgrTrimming, mgrFree, bins, 
                                   pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                   tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                   nstolen, cand, bsPtr, bsCtr, bsPred, bpp, 
                                   pm, ws, wforce, wa, qsz, qshard, qi, qpop, 
                                   qdef, qclaimed, gc, gk, gtail, gsz, sPtr, 
                                   sCtr, sPred, nxt, asz, ameta, ablk, 
                                   areserved, da, dmi, dtarget, ops, asel, 
                                   dsel, iops, iasel, idsel >>

RW_bpp(self) == /\ pc[self] = "RW_bpp"
                /\ IF FrontierMode = "packed"
                      THEN /\ bpp' = [bpp EXCEPT ![self] = hdrPrev[mPtr[cand[self]]]]
                      ELSE /\ TRUE
                           /\ bpp' = bpp
                /\ pc' = [pc EXCEPT ![self] = "RW_bcas"]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                cand, bsPtr, bsCtr, bsPred, pm, ws, wforce, wa, 
                                qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                gtail, gsz, sPtr, sCtr, sPred, nxt, asz, ameta, 
                                ablk, areserved, da, dmi, dtarget, ops, asel, 
                                dsel, iops, iasel, idsel >>

RW_bcas(self) == /\ pc[self] = "RW_bcas"
                 /\ IF fwPtr = bsPtr[self] /\ fwCtr = bsCtr[self] /\ fwPred = bsPred[self]
                       THEN /\ fwPtr' = mPtr[cand[self]]
                            /\ fwCtr' = (IF FrontierMode = "packed" THEN fwCtr ELSE fwCtr + 1)
                            /\ fwPred' = (IF FrontierMode = "packed" THEN bpp[self] ELSE NULL)
                            /\ pc' = [pc EXCEPT ![self] = "RW_brecycle"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "RW_brestore"]
                            /\ UNCHANGED << fwPtr, fwCtr, fwPred >>
                 /\ UNCHANGED << hdrMeta, hdrSize, hdrPrev, mState, mPtr, 
                                 mSize, mShard, metaCount, mgrActive, 
                                 mgrTrimming, mgrFree, bins, pending, 
                                 pendingCount, windowOpen, reclaimBusy, 
                                 guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                 live, held, stolenOutstanding, stack, rm, 
                                 tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

RW_brecycle(self) == /\ pc[self] = "RW_brecycle"
                     /\ batch' = [batch EXCEPT ![self] = batch[self] \ {cand[self]}]
                     /\ /\ rm' = [rm EXCEPT ![self] = cand[self]]
                        /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "RecycleMetadata",
                                                                 pc        |->  "RW_bload",
                                                                 rm        |->  rm[self] ] >>
                                                             \o stack[self]]
                     /\ pc' = [pc EXCEPT ![self] = "RM_state"]
                     /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                     hdrPrev, mState, mPtr, mSize, mShard, 
                                     metaCount, mgrActive, mgrTrimming, 
                                     mgrFree, bins, pending, pendingCount, 
                                     windowOpen, reclaimBusy, guardDepth, 
                                     irqActive, rvMeta, rvIdx, rvBlk, live, 
                                     held, stolenOutstanding, tPtr, tCtr, 
                                     tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                     rs, stolen, nstolen, cand, bsPtr, bsCtr, 
                                     bsPred, bpp, pm, ws, wforce, wa, qsz, 
                                     qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                     gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                     ameta, ablk, areserved, da, dmi, dtarget, 
                                     ops, asel, dsel, iops, iasel, idsel >>

RW_brestore(self) == /\ pc[self] = "RW_brestore"
                     /\ IF mState[cand[self]] = "FRONTIER_RECLAIM_IN_PROGRESS"
                           THEN /\ mState' = [mState EXCEPT ![cand[self]] = "DEALLOCATED_PENDING"]
                           ELSE /\ TRUE
                                /\ UNCHANGED mState
                     /\ pc' = [pc EXCEPT ![self] = "RW_bload"]
                     /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                     hdrPrev, mPtr, mSize, mShard, metaCount, 
                                     mgrActive, mgrTrimming, mgrFree, bins, 
                                     pending, pendingCount, windowOpen, 
                                     reclaimBusy, guardDepth, irqActive, 
                                     rvMeta, rvIdx, rvBlk, live, held, 
                                     stolenOutstanding, stack, rm, tPtr, tCtr, 
                                     tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                     rs, stolen, batch, nstolen, cand, bsPtr, 
                                     bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                     qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                     gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                     asz, ameta, ablk, areserved, da, dmi, 
                                     dtarget, ops, asel, dsel, iops, iasel, 
                                     idsel >>

RW_bdone(self) == /\ pc[self] = "RW_bdone"
                  /\ IF SerializeReclaim
                        THEN /\ reclaimBusy' = FALSE
                        ELSE /\ TRUE
                             /\ UNCHANGED reclaimBusy
                  /\ pc' = [pc EXCEPT ![self] = "RW_publish"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                  live, held, stolenOutstanding, stack, rm, 
                                  tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                  xshrunk, rs, stolen, batch, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                  wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                  gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                  asz, ameta, ablk, areserved, da, dmi, 
                                  dtarget, ops, asel, dsel, iops, iasel, idsel >>

RW_publish(self) == /\ pc[self] = "RW_publish"
                    /\ IF batch[self] # {}
                          THEN /\ pm' = [pm EXCEPT ![self] = Min(batch[self])]
                               /\ batch' = [batch EXCEPT ![self] = batch[self] \ {Min(batch[self])}]
                               /\ pc' = [pc EXCEPT ![self] = "RW_pub_state"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "RW_chunk"]
                               /\ UNCHANGED << batch, pm >>
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrActive, mgrTrimming, mgrFree, 
                                    bins, pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, stack, rm, tPtr, tCtr, 
                                    tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                    rs, stolen, nstolen, cand, bsPtr, bsCtr, 
                                    bsPred, bpp, ws, wforce, wa, qsz, qshard, 
                                    qi, qpop, qdef, qclaimed, gc, gk, gtail, 
                                    gsz, sPtr, sCtr, sPred, nxt, asz, ameta, 
                                    ablk, areserved, da, dmi, dtarget, ops, 
                                    asel, dsel, iops, iasel, idsel >>

RW_pub_state(self) == /\ pc[self] = "RW_pub_state"
                      /\ IF mState[pm[self]] = "DEALLOCATED_PENDING"
                            THEN /\ mState' = [mState EXCEPT ![pm[self]] = "AVAILABLE"]
                                 /\ pc' = [pc EXCEPT ![self] = "RW_pub_push"]
                            ELSE /\ pc' = [pc EXCEPT ![self] = "RW_publish"]
                                 /\ UNCHANGED mState
                      /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                      hdrPrev, mPtr, mSize, mShard, metaCount, 
                                      mgrActive, mgrTrimming, mgrFree, bins, 
                                      pending, pendingCount, windowOpen, 
                                      reclaimBusy, guardDepth, irqActive, 
                                      rvMeta, rvIdx, rvBlk, live, held, 
                                      stolenOutstanding, stack, rm, tPtr, tCtr, 
                                      tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                      rs, stolen, batch, nstolen, cand, bsPtr, 
                                      bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                      qsz, qshard, qi, qpop, qdef, qclaimed, 
                                      gc, gk, gtail, gsz, sPtr, sCtr, sPred, 
                                      nxt, asz, ameta, ablk, areserved, da, 
                                      dmi, dtarget, ops, asel, dsel, iops, 
                                      iasel, idsel >>

RW_pub_push(self) == /\ pc[self] = "RW_pub_push"
                     /\ Assert(~InAnyList(pm[self]), 
                               "Failure of assertion at line 322, column 13.")
                     /\ bins' = [bins EXCEPT ![mSize[pm[self]]][mShard[pm[self]]] = bins[mSize[pm[self]]][mShard[pm[self]]] \cup {pm[self]}]
                     /\ pc' = [pc EXCEPT ![self] = "RW_publish"]
                     /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                     hdrPrev, mState, mPtr, mSize, mShard, 
                                     metaCount, mgrActive, mgrTrimming, 
                                     mgrFree, pending, pendingCount, 
                                     windowOpen, reclaimBusy, guardDepth, 
                                     irqActive, rvMeta, rvIdx, rvBlk, live, 
                                     held, stolenOutstanding, stack, rm, tPtr, 
                                     tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                     xshrunk, rs, stolen, batch, nstolen, cand, 
                                     bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                     wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                     gc, gk, gtail, gsz, sPtr, sCtr, sPred, 
                                     nxt, asz, ameta, ablk, areserved, da, dmi, 
                                     dtarget, ops, asel, dsel, iops, iasel, 
                                     idsel >>

RW_reduce(self) == /\ pc[self] = "RW_reduce"
                   /\ Assert(PendingCountFirst => pendingCount[rs[self]] >= nstolen[self], 
                             "Failure of assertion at line 327, column 5.")
                   /\ pendingCount' = [pendingCount EXCEPT ![rs[self]] = IF pendingCount[rs[self]] > nstolen[self] THEN pendingCount[rs[self]] - nstolen[self] ELSE 0]
                   /\ stolenOutstanding' = [stolenOutstanding EXCEPT ![rs[self]] = stolenOutstanding[rs[self]] - nstolen[self]]
                   /\ pc' = [pc EXCEPT ![self] = "RW_steal"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, windowOpen, reclaimBusy, 
                                   guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                   live, held, stack, rm, tPtr, tCtr, tPred, 
                                   tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                   stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                   bsPred, bpp, pm, ws, wforce, wa, qsz, 
                                   qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                   gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                   ameta, ablk, areserved, da, dmi, dtarget, 
                                   ops, asel, dsel, iops, iasel, idsel >>

RW_tail(self) == /\ pc[self] = "RW_tail"
                 /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "TryReclaim",
                                                          pc        |->  "RW_trim",
                                                          tPtr      |->  tPtr[self],
                                                          tCtr      |->  tCtr[self],
                                                          tPred     |->  tPred[self],
                                                          tPrev     |->  tPrev[self],
                                                          tMi       |->  tMi[self],
                                                          tPP       |->  tPP[self] ] >>
                                                      \o stack[self]]
                 /\ tPtr' = [tPtr EXCEPT ![self] = 0]
                 /\ tCtr' = [tCtr EXCEPT ![self] = 0]
                 /\ tPred' = [tPred EXCEPT ![self] = NULL]
                 /\ tPrev' = [tPrev EXCEPT ![self] = NULL]
                 /\ tMi' = [tMi EXCEPT ![self] = NULL]
                 /\ tPP' = [tPP EXCEPT ![self] = NULL]
                 /\ pc' = [pc EXCEPT ![self] = "TR_flag"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 rm, xc, xk, xshrunk, rs, stolen, batch, 
                                 nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, 
                                 ws, wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                 qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                 sPred, nxt, asz, ameta, ablk, areserved, da, 
                                 dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                 idsel >>

RW_trim(self) == /\ pc[self] = "RW_trim"
                 /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Trim",
                                                          pc        |->  "RW_ret",
                                                          xc        |->  xc[self],
                                                          xk        |->  xk[self],
                                                          xshrunk   |->  xshrunk[self] ] >>
                                                      \o stack[self]]
                 /\ xc' = [xc EXCEPT ![self] = 0]
                 /\ xk' = [xk EXCEPT ![self] = 0]
                 /\ xshrunk' = [xshrunk EXCEPT ![self] = FALSE]
                 /\ pc' = [pc EXCEPT ![self] = "TM_load"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, rs, 
                                 stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                 bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                 qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                 sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                 areserved, da, dmi, dtarget, ops, asel, dsel, 
                                 iops, iasel, idsel >>

RW_ret(self) == /\ pc[self] = "RW_ret"
                /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                /\ stolen' = [stolen EXCEPT ![self] = Head(stack[self]).stolen]
                /\ batch' = [batch EXCEPT ![self] = Head(stack[self]).batch]
                /\ nstolen' = [nstolen EXCEPT ![self] = Head(stack[self]).nstolen]
                /\ cand' = [cand EXCEPT ![self] = Head(stack[self]).cand]
                /\ bsPtr' = [bsPtr EXCEPT ![self] = Head(stack[self]).bsPtr]
                /\ bsCtr' = [bsCtr EXCEPT ![self] = Head(stack[self]).bsCtr]
                /\ bsPred' = [bsPred EXCEPT ![self] = Head(stack[self]).bsPred]
                /\ bpp' = [bpp EXCEPT ![self] = Head(stack[self]).bpp]
                /\ pm' = [pm EXCEPT ![self] = Head(stack[self]).pm]
                /\ rs' = [rs EXCEPT ![self] = Head(stack[self]).rs]
                /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                xshrunk, ws, wforce, wa, qsz, qshard, qi, qpop, 
                                qdef, qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                sPred, nxt, asz, ameta, ablk, areserved, da, 
                                dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                idsel >>

RunWindow(self) == RW_steal(self) \/ RW_chunk(self) \/ RW_bflag(self)
                      \/ RW_bload(self) \/ RW_bfind(self)
                      \/ RW_bclaim(self) \/ RW_bpp(self) \/ RW_bcas(self)
                      \/ RW_brecycle(self) \/ RW_brestore(self)
                      \/ RW_bdone(self) \/ RW_publish(self)
                      \/ RW_pub_state(self) \/ RW_pub_push(self)
                      \/ RW_reduce(self) \/ RW_tail(self) \/ RW_trim(self)
                      \/ RW_ret(self)

MW_count(self) == /\ pc[self] = "MW_count"
                  /\ IF pendingCount[ws[self]] = 0 \/ (~wforce[self] /\ pendingCount[ws[self]] < Threshold)
                        THEN /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                             /\ ws' = [ws EXCEPT ![self] = Head(stack[self]).ws]
                             /\ wforce' = [wforce EXCEPT ![self] = Head(stack[self]).wforce]
                             /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "MW_open"]
                             /\ UNCHANGED << stack, ws, wforce >>
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, 
                                  xk, xshrunk, rs, stolen, batch, nstolen, 
                                  cand, bsPtr, bsCtr, bsPred, bpp, pm, wa, qsz, 
                                  qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                  gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                  ameta, ablk, areserved, da, dmi, dtarget, 
                                  ops, asel, dsel, iops, iasel, idsel >>

MW_open(self) == /\ pc[self] = "MW_open"
                 /\ IF windowOpen[ws[self]]
                       THEN /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                            /\ ws' = [ws EXCEPT ![self] = Head(stack[self]).ws]
                            /\ wforce' = [wforce EXCEPT ![self] = Head(stack[self]).wforce]
                            /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                            /\ UNCHANGED windowOpen
                       ELSE /\ windowOpen' = [windowOpen EXCEPT ![ws[self]] = TRUE]
                            /\ pc' = [pc EXCEPT ![self] = "MW_run"]
                            /\ UNCHANGED << stack, ws, wforce >>
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, reclaimBusy, 
                                 guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                 live, held, stolenOutstanding, rm, tPtr, tCtr, 
                                 tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                 stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                 bsPred, bpp, pm, wa, qsz, qshard, qi, qpop, 
                                 qdef, qclaimed, gc, gk, gtail, gsz, sPtr, 
                                 sCtr, sPred, nxt, asz, ameta, ablk, areserved, 
                                 da, dmi, dtarget, ops, asel, dsel, iops, 
                                 iasel, idsel >>

MW_run(self) == /\ pc[self] = "MW_run"
                /\ /\ rs' = [rs EXCEPT ![self] = ws[self]]
                   /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "RunWindow",
                                                            pc        |->  "MW_close",
                                                            stolen    |->  stolen[self],
                                                            batch     |->  batch[self],
                                                            nstolen   |->  nstolen[self],
                                                            cand      |->  cand[self],
                                                            bsPtr     |->  bsPtr[self],
                                                            bsCtr     |->  bsCtr[self],
                                                            bsPred    |->  bsPred[self],
                                                            bpp       |->  bpp[self],
                                                            pm        |->  pm[self],
                                                            rs        |->  rs[self] ] >>
                                                        \o stack[self]]
                /\ stolen' = [stolen EXCEPT ![self] = <<>>]
                /\ batch' = [batch EXCEPT ![self] = {}]
                /\ nstolen' = [nstolen EXCEPT ![self] = 0]
                /\ cand' = [cand EXCEPT ![self] = NULL]
                /\ bsPtr' = [bsPtr EXCEPT ![self] = 0]
                /\ bsCtr' = [bsCtr EXCEPT ![self] = 0]
                /\ bsPred' = [bsPred EXCEPT ![self] = NULL]
                /\ bpp' = [bpp EXCEPT ![self] = NULL]
                /\ pm' = [pm EXCEPT ![self] = NULL]
                /\ pc' = [pc EXCEPT ![self] = "RW_steal"]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                xshrunk, ws, wforce, wa, qsz, qshard, qi, qpop, 
                                qdef, qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                sPred, nxt, asz, ameta, ablk, areserved, da, 
                                dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                idsel >>

MW_close(self) == /\ pc[self] = "MW_close"
                  /\ windowOpen' = [windowOpen EXCEPT ![ws[self]] = FALSE]
                  /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                  /\ ws' = [ws EXCEPT ![self] = Head(stack[self]).ws]
                  /\ wforce' = [wforce EXCEPT ![self] = Head(stack[self]).wforce]
                  /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, reclaimBusy, 
                                  guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                  live, held, stolenOutstanding, rm, tPtr, 
                                  tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                  xshrunk, rs, stolen, batch, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, wa, qsz, 
                                  qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                  gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                  ameta, ablk, areserved, da, dmi, dtarget, 
                                  ops, asel, dsel, iops, iasel, idsel >>

MaybeWindow(self) == MW_count(self) \/ MW_open(self) \/ MW_run(self)
                        \/ MW_close(self)

WA_loop(self) == /\ pc[self] = "WA_loop"
                 /\ IF wa[self] <= NumCPUs
                       THEN /\ /\ rs' = [rs EXCEPT ![self] = wa[self]]
                               /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "RunWindow",
                                                                        pc        |->  "WA_next",
                                                                        stolen    |->  stolen[self],
                                                                        batch     |->  batch[self],
                                                                        nstolen   |->  nstolen[self],
                                                                        cand      |->  cand[self],
                                                                        bsPtr     |->  bsPtr[self],
                                                                        bsCtr     |->  bsCtr[self],
                                                                        bsPred    |->  bsPred[self],
                                                                        bpp       |->  bpp[self],
                                                                        pm        |->  pm[self],
                                                                        rs        |->  rs[self] ] >>
                                                                    \o stack[self]]
                            /\ stolen' = [stolen EXCEPT ![self] = <<>>]
                            /\ batch' = [batch EXCEPT ![self] = {}]
                            /\ nstolen' = [nstolen EXCEPT ![self] = 0]
                            /\ cand' = [cand EXCEPT ![self] = NULL]
                            /\ bsPtr' = [bsPtr EXCEPT ![self] = 0]
                            /\ bsCtr' = [bsCtr EXCEPT ![self] = 0]
                            /\ bsPred' = [bsPred EXCEPT ![self] = NULL]
                            /\ bpp' = [bpp EXCEPT ![self] = NULL]
                            /\ pm' = [pm EXCEPT ![self] = NULL]
                            /\ pc' = [pc EXCEPT ![self] = "RW_steal"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "WA_ret"]
                            /\ UNCHANGED << stack, rs, stolen, batch, nstolen, 
                                            cand, bsPtr, bsCtr, bsPred, bpp, 
                                            pm >>
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, 
                                 xk, xshrunk, ws, wforce, wa, qsz, qshard, qi, 
                                 qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                 sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                 areserved, da, dmi, dtarget, ops, asel, dsel, 
                                 iops, iasel, idsel >>

WA_next(self) == /\ pc[self] = "WA_next"
                 /\ wa' = [wa EXCEPT ![self] = wa[self] + 1]
                 /\ pc' = [pc EXCEPT ![self] = "WA_loop"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                 xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                 cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                 wforce, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                 gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                 asz, ameta, ablk, areserved, da, dmi, dtarget, 
                                 ops, asel, dsel, iops, iasel, idsel >>

WA_ret(self) == /\ pc[self] = "WA_ret"
                /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                /\ wa' = [wa EXCEPT ![self] = Head(stack[self]).wa]
                /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                xshrunk, rs, stolen, batch, nstolen, cand, 
                                bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, qsz, 
                                qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                gtail, gsz, sPtr, sCtr, sPred, nxt, asz, ameta, 
                                ablk, areserved, da, dmi, dtarget, ops, asel, 
                                dsel, iops, iasel, idsel >>

RunWindowAll(self) == WA_loop(self) \/ WA_next(self) \/ WA_ret(self)

SR_restart(self) == /\ pc[self] = "SR_restart"
                    /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] + 1]
                    /\ qi' = [qi EXCEPT ![self] = 1]
                    /\ qdef' = [qdef EXCEPT ![self] = NULL]
                    /\ qclaimed' = [qclaimed EXCEPT ![self] = NULL]
                    /\ pc' = [pc EXCEPT ![self] = "SR_scan"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrActive, mgrTrimming, mgrFree, 
                                    bins, pending, pendingCount, windowOpen, 
                                    reclaimBusy, irqActive, rvMeta, rvIdx, 
                                    rvBlk, live, held, stolenOutstanding, 
                                    stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                    tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                    nstolen, cand, bsPtr, bsCtr, bsPred, bpp, 
                                    pm, ws, wforce, wa, qsz, qshard, qpop, gc, 
                                    gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                    asz, ameta, ablk, areserved, da, dmi, 
                                    dtarget, ops, asel, dsel, iops, iasel, 
                                    idsel >>

SR_scan(self) == /\ pc[self] = "SR_scan"
                 /\ IF qi[self] <= NumCPUs
                       THEN /\ pc' = [pc EXCEPT ![self] = "SR_pop"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "SR_exit"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                 xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                 cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                 wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                 qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                 sPred, nxt, asz, ameta, ablk, areserved, da, 
                                 dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                 idsel >>

SR_pop(self) == /\ pc[self] = "SR_pop"
                /\ LET sh == ShardOrder(qshard[self])[qi[self]] IN
                     IF bins[qsz[self]][sh] = {}
                        THEN /\ qpop' = [qpop EXCEPT ![self] = NULL]
                             /\ bins' = bins
                        ELSE /\ \E x \in bins[qsz[self]][sh]:
                                  /\ qpop' = [qpop EXCEPT ![self] = x]
                                  /\ bins' = [bins EXCEPT ![qsz[self]][sh] = bins[qsz[self]][sh] \ {x}]
                /\ IF qpop'[self] = NULL
                      THEN /\ qi' = [qi EXCEPT ![self] = qi[self] + 1]
                           /\ pc' = [pc EXCEPT ![self] = "SR_scan"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "SR_claim"]
                           /\ qi' = qi
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                pending, pendingCount, windowOpen, reclaimBusy, 
                                guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                live, held, stolenOutstanding, stack, rm, tPtr, 
                                tCtr, tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                rs, stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                qdef, qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                sPred, nxt, asz, ameta, ablk, areserved, da, 
                                dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                idsel >>

SR_claim(self) == /\ pc[self] = "SR_claim"
                  /\ IF mState[qpop[self]] = "AVAILABLE"
                        THEN /\ mState' = [mState EXCEPT ![qpop[self]] = "LOCKED"]
                             /\ qclaimed' = [qclaimed EXCEPT ![self] = qpop[self]]
                             /\ pc' = [pc EXCEPT ![self] = "SR_exit"]
                             /\ UNCHANGED << qi, qdef >>
                        ELSE /\ IF mState[qpop[self]] = "FRONTIER_RECLAIM_IN_PROGRESS"
                                   THEN /\ pc' = [pc EXCEPT ![self] = "SR_repush"]
                                        /\ UNCHANGED << qi, qdef >>
                                   ELSE /\ IF mState[qpop[self]] = "FRONTIER_RECLAIMED"
                                              THEN /\ qdef' = [qdef EXCEPT ![self] = qpop[self]]
                                                   /\ pc' = [pc EXCEPT ![self] = "SR_exit"]
                                                   /\ qi' = qi
                                              ELSE /\ qi' = [qi EXCEPT ![self] = qi[self] + 1]
                                                   /\ pc' = [pc EXCEPT ![self] = "SR_scan"]
                                                   /\ qdef' = qdef
                             /\ UNCHANGED << mState, qclaimed >>
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mPtr, mSize, mShard, metaCount, 
                                  mgrActive, mgrTrimming, mgrFree, bins, 
                                  pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                  nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, 
                                  ws, wforce, wa, qsz, qshard, qpop, gc, gk, 
                                  gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                  ameta, ablk, areserved, da, dmi, dtarget, 
                                  ops, asel, dsel, iops, iasel, idsel >>

SR_repush(self) == /\ pc[self] = "SR_repush"
                   /\ Assert(~InAnyList(qpop[self]), 
                             "Failure of assertion at line 388, column 9.")
                   /\ LET sh == ShardOrder(qshard[self])[qi[self]] IN
                        bins' = [bins EXCEPT ![qsz[self]][sh] = bins[qsz[self]][sh] \cup {qpop[self]}]
                   /\ qi' = [qi EXCEPT ![self] = qi[self] + 1]
                   /\ pc' = [pc EXCEPT ![self] = "SR_scan"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                   tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                   nstolen, cand, bsPtr, bsCtr, bsPred, bpp, 
                                   pm, ws, wforce, wa, qsz, qshard, qpop, qdef, 
                                   qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                   sPred, nxt, asz, ameta, ablk, areserved, da, 
                                   dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                   idsel >>

SR_exit(self) == /\ pc[self] = "SR_exit"
                 /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] - 1]
                 /\ IF qdef[self] # NULL
                       THEN /\ /\ rm' = [rm EXCEPT ![self] = qdef[self]]
                               /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "RecycleMetadata",
                                                                        pc        |->  "SR_restart",
                                                                        rm        |->  rm[self] ] >>
                                                                    \o stack[self]]
                            /\ pc' = [pc EXCEPT ![self] = "RM_state"]
                            /\ UNCHANGED << rvMeta, qsz, qshard, qi, qpop, 
                                            qdef, qclaimed >>
                       ELSE /\ IF qclaimed[self] # NULL
                                  THEN /\ rvMeta' = [rvMeta EXCEPT ![self] = qclaimed[self]]
                                       /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                       /\ qi' = [qi EXCEPT ![self] = Head(stack[self]).qi]
                                       /\ qpop' = [qpop EXCEPT ![self] = Head(stack[self]).qpop]
                                       /\ qdef' = [qdef EXCEPT ![self] = Head(stack[self]).qdef]
                                       /\ qclaimed' = [qclaimed EXCEPT ![self] = Head(stack[self]).qclaimed]
                                       /\ qsz' = [qsz EXCEPT ![self] = Head(stack[self]).qsz]
                                       /\ qshard' = [qshard EXCEPT ![self] = Head(stack[self]).qshard]
                                       /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                                  ELSE /\ rvMeta' = [rvMeta EXCEPT ![self] = NULL]
                                       /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                       /\ qi' = [qi EXCEPT ![self] = Head(stack[self]).qi]
                                       /\ qpop' = [qpop EXCEPT ![self] = Head(stack[self]).qpop]
                                       /\ qdef' = [qdef EXCEPT ![self] = Head(stack[self]).qdef]
                                       /\ qclaimed' = [qclaimed EXCEPT ![self] = Head(stack[self]).qclaimed]
                                       /\ qsz' = [qsz EXCEPT ![self] = Head(stack[self]).qsz]
                                       /\ qshard' = [qshard EXCEPT ![self] = Head(stack[self]).qshard]
                                       /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                            /\ rm' = rm
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, irqActive, rvIdx, rvBlk, live, 
                                 held, stolenOutstanding, tPtr, tCtr, tPred, 
                                 tPrev, tMi, tPP, xc, xk, xshrunk, rs, stolen, 
                                 batch, nstolen, cand, bsPtr, bsCtr, bsPred, 
                                 bpp, pm, ws, wforce, wa, gc, gk, gtail, gsz, 
                                 sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                 areserved, da, dmi, dtarget, ops, asel, dsel, 
                                 iops, iasel, idsel >>

Search(self) == SR_restart(self) \/ SR_scan(self) \/ SR_pop(self)
                   \/ SR_claim(self) \/ SR_repush(self) \/ SR_exit(self)

GM_load(self) == /\ pc[self] = "GM_load"
                 /\ gc' = [gc EXCEPT ![self] = metaCount]
                 /\ IF metaCount = 0
                       THEN /\ pc' = [pc EXCEPT ![self] = "GM_bump"]
                            /\ UNCHANGED << gk, gtail >>
                       ELSE /\ gtail' = [gtail EXCEPT ![self] = (metaCount - 1) \div RecordsPerManager]
                            /\ gk' = [gk EXCEPT ![self] = 0]
                            /\ pc' = [pc EXCEPT ![self] = "GM_scan"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                 xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                 cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                 wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                 qclaimed, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

GM_scan(self) == /\ pc[self] = "GM_scan"
                 /\ IF gk[self] <= gtail[self]
                       THEN /\ pc' = [pc EXCEPT ![self] = "GM_reserve"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "GM_bump"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                 xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                 cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                 wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                 qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                 sPred, nxt, asz, ameta, ablk, areserved, da, 
                                 dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                 idsel >>

GM_reserve(self) == /\ pc[self] = "GM_reserve"
                    /\ IF mgrTrimming[gk[self]]
                          THEN /\ gk' = [gk EXCEPT ![self] = gk[self] + 1]
                               /\ pc' = [pc EXCEPT ![self] = "GM_scan"]
                               /\ UNCHANGED mgrActive
                          ELSE /\ mgrActive' = [mgrActive EXCEPT ![gk[self]] = mgrActive[gk[self]] + 1]
                               /\ pc' = [pc EXCEPT ![self] = "GM_pop"]
                               /\ gk' = gk
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrTrimming, mgrFree, bins, 
                                    pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, stack, rm, tPtr, tCtr, 
                                    tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                    rs, stolen, batch, nstolen, cand, bsPtr, 
                                    bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                    qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                    gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                    ameta, ablk, areserved, da, dmi, dtarget, 
                                    ops, asel, dsel, iops, iasel, idsel >>

GM_pop(self) == /\ pc[self] = "GM_pop"
                /\ IF mgrFree[gk[self]] # {}
                      THEN /\ \E x \in mgrFree[gk[self]]:
                                /\ mgrFree' = [mgrFree EXCEPT ![gk[self]] = mgrFree[gk[self]] \ {x}]
                                /\ rvIdx' = [rvIdx EXCEPT ![self] = x]
                           /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                           /\ gc' = [gc EXCEPT ![self] = Head(stack[self]).gc]
                           /\ gk' = [gk EXCEPT ![self] = Head(stack[self]).gk]
                           /\ gtail' = [gtail EXCEPT ![self] = Head(stack[self]).gtail]
                           /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "GM_release"]
                           /\ UNCHANGED << mgrFree, rvIdx, stack, gc, gk, 
                                           gtail >>
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, bins, 
                                pending, pendingCount, windowOpen, reclaimBusy, 
                                guardDepth, irqActive, rvMeta, rvBlk, live, 
                                held, stolenOutstanding, rm, tPtr, tCtr, tPred, 
                                tPrev, tMi, tPP, xc, xk, xshrunk, rs, stolen, 
                                batch, nstolen, cand, bsPtr, bsCtr, bsPred, 
                                bpp, pm, ws, wforce, wa, qsz, qshard, qi, qpop, 
                                qdef, qclaimed, gsz, sPtr, sCtr, sPred, nxt, 
                                asz, ameta, ablk, areserved, da, dmi, dtarget, 
                                ops, asel, dsel, iops, iasel, idsel >>

GM_release(self) == /\ pc[self] = "GM_release"
                    /\ mgrActive' = [mgrActive EXCEPT ![gk[self]] = mgrActive[gk[self]] - 1]
                    /\ gk' = [gk EXCEPT ![self] = gk[self] + 1]
                    /\ pc' = [pc EXCEPT ![self] = "GM_scan"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrTrimming, mgrFree, bins, 
                                    pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, stack, rm, tPtr, tCtr, 
                                    tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                    rs, stolen, batch, nstolen, cand, bsPtr, 
                                    bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                    qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                    gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                    ameta, ablk, areserved, da, dmi, dtarget, 
                                    ops, asel, dsel, iops, iasel, idsel >>

GM_bump(self) == /\ pc[self] = "GM_bump"
                 /\ IF gc[self] \div RecordsPerManager >= NumManagers
                       THEN /\ rvIdx' = [rvIdx EXCEPT ![self] = NULL]
                            /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                            /\ gc' = [gc EXCEPT ![self] = Head(stack[self]).gc]
                            /\ gk' = [gk EXCEPT ![self] = Head(stack[self]).gk]
                            /\ gtail' = [gtail EXCEPT ![self] = Head(stack[self]).gtail]
                            /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                       ELSE /\ gk' = [gk EXCEPT ![self] = gc[self] \div RecordsPerManager]
                            /\ pc' = [pc EXCEPT ![self] = "GM_breserve"]
                            /\ UNCHANGED << rvIdx, stack, gc, gtail >>
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvBlk, live, held, stolenOutstanding, rm, 
                                 tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gsz, 
                                 sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                 areserved, da, dmi, dtarget, ops, asel, dsel, 
                                 iops, iasel, idsel >>

GM_breserve(self) == /\ pc[self] = "GM_breserve"
                     /\ ~mgrTrimming[gk[self]] \/ metaCount # gc[self]
                     /\ IF mgrTrimming[gk[self]]
                           THEN /\ gc' = [gc EXCEPT ![self] = metaCount]
                                /\ pc' = [pc EXCEPT ![self] = "GM_bump"]
                                /\ UNCHANGED mgrActive
                           ELSE /\ mgrActive' = [mgrActive EXCEPT ![gk[self]] = mgrActive[gk[self]] + 1]
                                /\ pc' = [pc EXCEPT ![self] = "GM_bcheck"]
                                /\ gc' = gc
                     /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                     hdrPrev, mState, mPtr, mSize, mShard, 
                                     metaCount, mgrTrimming, mgrFree, bins, 
                                     pending, pendingCount, windowOpen, 
                                     reclaimBusy, guardDepth, irqActive, 
                                     rvMeta, rvIdx, rvBlk, live, held, 
                                     stolenOutstanding, stack, rm, tPtr, tCtr, 
                                     tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                     rs, stolen, batch, nstolen, cand, bsPtr, 
                                     bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                     qsz, qshard, qi, qpop, qdef, qclaimed, gk, 
                                     gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                     ameta, ablk, areserved, da, dmi, dtarget, 
                                     ops, asel, dsel, iops, iasel, idsel >>

GM_bcheck(self) == /\ pc[self] = "GM_bcheck"
                   /\ IF MS - (gc[self] + 1) <= fwPtr
                         THEN /\ pc' = [pc EXCEPT ![self] = "GM_bfail"]
                         ELSE /\ pc' = [pc EXCEPT ![self] = "GM_bcas"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                   tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                   nstolen, cand, bsPtr, bsCtr, bsPred, bpp, 
                                   pm, ws, wforce, wa, qsz, qshard, qi, qpop, 
                                   qdef, qclaimed, gc, gk, gtail, gsz, sPtr, 
                                   sCtr, sPred, nxt, asz, ameta, ablk, 
                                   areserved, da, dmi, dtarget, ops, asel, 
                                   dsel, iops, iasel, idsel >>

GM_bcas(self) == /\ pc[self] = "GM_bcas"
                 /\ IF metaCount = gc[self]
                       THEN /\ metaCount' = gc[self] + 1
                            /\ IF ~FixedBoundary
                                  THEN /\ rvIdx' = [rvIdx EXCEPT ![self] = gc[self]]
                                       /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                       /\ gc' = [gc EXCEPT ![self] = Head(stack[self]).gc]
                                       /\ gk' = [gk EXCEPT ![self] = Head(stack[self]).gk]
                                       /\ gtail' = [gtail EXCEPT ![self] = Head(stack[self]).gtail]
                                       /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                                  ELSE /\ pc' = [pc EXCEPT ![self] = "GM_brecheck"]
                                       /\ UNCHANGED << rvIdx, stack, gc, gk, 
                                                       gtail >>
                       ELSE /\ gc' = [gc EXCEPT ![self] = metaCount]
                            /\ pc' = [pc EXCEPT ![self] = "GM_brelease"]
                            /\ UNCHANGED << metaCount, rvIdx, stack, gk, gtail >>
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 mgrActive, mgrTrimming, mgrFree, bins, 
                                 pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvBlk, live, held, stolenOutstanding, rm, 
                                 tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gsz, 
                                 sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                 areserved, da, dmi, dtarget, ops, asel, dsel, 
                                 iops, iasel, idsel >>

GM_brecheck(self) == /\ pc[self] = "GM_brecheck"
                     /\ IF MS - (gc[self] + 1) > fwPtr
                           THEN /\ rvIdx' = [rvIdx EXCEPT ![self] = gc[self]]
                                /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                /\ gc' = [gc EXCEPT ![self] = Head(stack[self]).gc]
                                /\ gk' = [gk EXCEPT ![self] = Head(stack[self]).gk]
                                /\ gtail' = [gtail EXCEPT ![self] = Head(stack[self]).gtail]
                                /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                           ELSE /\ pc' = [pc EXCEPT ![self] = "GM_bundo"]
                                /\ UNCHANGED << rvIdx, stack, gc, gk, gtail >>
                     /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                     hdrPrev, mState, mPtr, mSize, mShard, 
                                     metaCount, mgrActive, mgrTrimming, 
                                     mgrFree, bins, pending, pendingCount, 
                                     windowOpen, reclaimBusy, guardDepth, 
                                     irqActive, rvMeta, rvBlk, live, held, 
                                     stolenOutstanding, rm, tPtr, tCtr, tPred, 
                                     tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                     stolen, batch, nstolen, cand, bsPtr, 
                                     bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                     qsz, qshard, qi, qpop, qdef, qclaimed, 
                                     gsz, sPtr, sCtr, sPred, nxt, asz, ameta, 
                                     ablk, areserved, da, dmi, dtarget, ops, 
                                     asel, dsel, iops, iasel, idsel >>

GM_bundo(self) == /\ pc[self] = "GM_bundo"
                  /\ IF metaCount = gc[self] + 1
                        THEN /\ metaCount' = gc[self]
                             /\ pc' = [pc EXCEPT ![self] = "GM_bfail"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "GM_brecheck"]
                             /\ UNCHANGED metaCount
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  mgrActive, mgrTrimming, mgrFree, bins, 
                                  pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  stack, rm, tPtr, tCtr, tPred, tPrev, tMi, 
                                  tPP, xc, xk, xshrunk, rs, stolen, batch, 
                                  nstolen, cand, bsPtr, bsCtr, bsPred, bpp, pm, 
                                  ws, wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                  sPred, nxt, asz, ameta, ablk, areserved, da, 
                                  dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                  idsel >>

GM_brelease(self) == /\ pc[self] = "GM_brelease"
                     /\ mgrActive' = [mgrActive EXCEPT ![gk[self]] = mgrActive[gk[self]] - 1]
                     /\ pc' = [pc EXCEPT ![self] = "GM_bump"]
                     /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                     hdrPrev, mState, mPtr, mSize, mShard, 
                                     metaCount, mgrTrimming, mgrFree, bins, 
                                     pending, pendingCount, windowOpen, 
                                     reclaimBusy, guardDepth, irqActive, 
                                     rvMeta, rvIdx, rvBlk, live, held, 
                                     stolenOutstanding, stack, rm, tPtr, tCtr, 
                                     tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                     rs, stolen, batch, nstolen, cand, bsPtr, 
                                     bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                     qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                     gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                     asz, ameta, ablk, areserved, da, dmi, 
                                     dtarget, ops, asel, dsel, iops, iasel, 
                                     idsel >>

GM_bfail(self) == /\ pc[self] = "GM_bfail"
                  /\ mgrActive' = [mgrActive EXCEPT ![gk[self]] = mgrActive[gk[self]] - 1]
                  /\ rvIdx' = [rvIdx EXCEPT ![self] = NULL]
                  /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                  /\ gc' = [gc EXCEPT ![self] = Head(stack[self]).gc]
                  /\ gk' = [gk EXCEPT ![self] = Head(stack[self]).gk]
                  /\ gtail' = [gtail EXCEPT ![self] = Head(stack[self]).gtail]
                  /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrTrimming, mgrFree, bins, 
                                  pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvBlk, live, held, stolenOutstanding, rm, 
                                  tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                  xshrunk, rs, stolen, batch, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                  wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                  gsz, sPtr, sCtr, sPred, nxt, asz, ameta, 
                                  ablk, areserved, da, dmi, dtarget, ops, asel, 
                                  dsel, iops, iasel, idsel >>

GetMetaIndex(self) == GM_load(self) \/ GM_scan(self) \/ GM_reserve(self)
                         \/ GM_pop(self) \/ GM_release(self)
                         \/ GM_bump(self) \/ GM_breserve(self)
                         \/ GM_bcheck(self) \/ GM_bcas(self)
                         \/ GM_brecheck(self) \/ GM_bundo(self)
                         \/ GM_brelease(self) \/ GM_bfail(self)

FB_load(self) == /\ pc[self] = "FB_load"
                 /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] + 1]
                 /\ sPtr' = [sPtr EXCEPT ![self] = fwPtr]
                 /\ sCtr' = [sCtr EXCEPT ![self] = fwCtr]
                 /\ sPred' = [sPred EXCEPT ![self] = fwPred]
                 /\ pc' = [pc EXCEPT ![self] = "FB_check"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, irqActive, rvMeta, rvIdx, rvBlk, 
                                 live, held, stolenOutstanding, stack, rm, 
                                 tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, nxt, asz, ameta, ablk, areserved, 
                                 da, dmi, dtarget, ops, asel, dsel, iops, 
                                 iasel, idsel >>

FB_check(self) == /\ pc[self] = "FB_check"
                  /\ IF sPtr[self] + gsz[self] >= MS - (metaCount + 1)
                        THEN /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] - 1]
                             /\ rvBlk' = [rvBlk EXCEPT ![self] = NULL]
                             /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                             /\ sPtr' = [sPtr EXCEPT ![self] = Head(stack[self]).sPtr]
                             /\ sCtr' = [sCtr EXCEPT ![self] = Head(stack[self]).sCtr]
                             /\ sPred' = [sPred EXCEPT ![self] = Head(stack[self]).sPred]
                             /\ nxt' = [nxt EXCEPT ![self] = Head(stack[self]).nxt]
                             /\ gsz' = [gsz EXCEPT ![self] = Head(stack[self]).gsz]
                             /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                        ELSE /\ nxt' = [nxt EXCEPT ![self] = sPtr[self] + gsz[self]]
                             /\ pc' = [pc EXCEPT ![self] = "FB_pre"]
                             /\ UNCHANGED << guardDepth, rvBlk, stack, gsz, 
                                             sPtr, sCtr, sPred >>
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, irqActive, rvMeta, rvIdx, live, 
                                  held, stolenOutstanding, rm, tPtr, tCtr, 
                                  tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                  stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                  bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                  qi, qpop, qdef, qclaimed, gc, gk, gtail, asz, 
                                  ameta, ablk, areserved, da, dmi, dtarget, 
                                  ops, asel, dsel, iops, iasel, idsel >>

FB_pre(self) == /\ pc[self] = "FB_pre"
                /\ IF FrontierMode = "pre_cas_backlink"
                      THEN /\ Assert(~InUserRegion(nxt[self]), 
                                     "Failure of assertion at line 466, column 9.")
                           /\ hdrPrev' = [hdrPrev EXCEPT ![nxt[self]] = sPtr[self]]
                      ELSE /\ TRUE
                           /\ UNCHANGED hdrPrev
                /\ pc' = [pc EXCEPT ![self] = "FB_cas"]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, mState, 
                                mPtr, mSize, mShard, metaCount, mgrActive, 
                                mgrTrimming, mgrFree, bins, pending, 
                                pendingCount, windowOpen, reclaimBusy, 
                                guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                live, held, stolenOutstanding, stack, rm, tPtr, 
                                tCtr, tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                rs, stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                areserved, da, dmi, dtarget, ops, asel, dsel, 
                                iops, iasel, idsel >>

FB_cas(self) == /\ pc[self] = "FB_cas"
                /\ IF fwPtr = sPtr[self] /\ fwCtr = sCtr[self] /\ fwPred = sPred[self]
                      THEN /\ fwPtr' = nxt[self]
                           /\ fwCtr' = (IF FrontierMode = "packed" THEN fwCtr ELSE fwCtr + 1)
                           /\ fwPred' = (IF FrontierMode = "packed" THEN sPtr[self] ELSE NULL)
                           /\ IF FrontierMode = "pre_cas_backlink"
                                 THEN /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] - 1]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED guardDepth
                           /\ IF ~FixedBoundary
                                 THEN /\ pc' = [pc EXCEPT ![self] = "FB_size"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "FB_recheck"]
                           /\ UNCHANGED << sPtr, sCtr, sPred >>
                      ELSE /\ sPtr' = [sPtr EXCEPT ![self] = fwPtr]
                           /\ sCtr' = [sCtr EXCEPT ![self] = fwCtr]
                           /\ sPred' = [sPred EXCEPT ![self] = fwPred]
                           /\ pc' = [pc EXCEPT ![self] = "FB_check"]
                           /\ UNCHANGED << fwPtr, fwCtr, fwPred, guardDepth >>
                /\ UNCHANGED << hdrMeta, hdrSize, hdrPrev, mState, mPtr, mSize, 
                                mShard, metaCount, mgrActive, mgrTrimming, 
                                mgrFree, bins, pending, pendingCount, 
                                windowOpen, reclaimBusy, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                qclaimed, gc, gk, gtail, gsz, nxt, asz, ameta, 
                                ablk, areserved, da, dmi, dtarget, ops, asel, 
                                dsel, iops, iasel, idsel >>

FB_recheck(self) == /\ pc[self] = "FB_recheck"
                    /\ IF nxt[self] < MS - (metaCount + 1)
                          THEN /\ pc' = [pc EXCEPT ![self] = "FB_size"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "FB_undo"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrActive, mgrTrimming, mgrFree, 
                                    bins, pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, stack, rm, tPtr, tCtr, 
                                    tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                    rs, stolen, batch, nstolen, cand, bsPtr, 
                                    bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                    qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                    gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                    asz, ameta, ablk, areserved, da, dmi, 
                                    dtarget, ops, asel, dsel, iops, iasel, 
                                    idsel >>

FB_undo(self) == /\ pc[self] = "FB_undo"
                 /\ IF fwPtr = nxt[self] /\ fwPred = sPtr[self] /\ fwCtr = sCtr[self]
                       THEN /\ fwPtr' = sPtr[self]
                            /\ fwPred' = sPred[self]
                            /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] - 1]
                            /\ rvBlk' = [rvBlk EXCEPT ![self] = NULL]
                            /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                            /\ sPtr' = [sPtr EXCEPT ![self] = Head(stack[self]).sPtr]
                            /\ sCtr' = [sCtr EXCEPT ![self] = Head(stack[self]).sCtr]
                            /\ sPred' = [sPred EXCEPT ![self] = Head(stack[self]).sPred]
                            /\ nxt' = [nxt EXCEPT ![self] = Head(stack[self]).nxt]
                            /\ gsz' = [gsz EXCEPT ![self] = Head(stack[self]).gsz]
                            /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                       ELSE /\ IF nxt[self] < MS - (metaCount + 1)
                                  THEN /\ pc' = [pc EXCEPT ![self] = "FB_size"]
                                  ELSE /\ pc' = [pc EXCEPT ![self] = "FB_undo"]
                            /\ UNCHANGED << fwPtr, fwPred, guardDepth, rvBlk, 
                                            stack, gsz, sPtr, sCtr, sPred, nxt >>
                 /\ UNCHANGED << fwCtr, hdrMeta, hdrSize, hdrPrev, mState, 
                                 mPtr, mSize, mShard, metaCount, mgrActive, 
                                 mgrTrimming, mgrFree, bins, pending, 
                                 pendingCount, windowOpen, reclaimBusy, 
                                 irqActive, rvMeta, rvIdx, live, held, 
                                 stolenOutstanding, rm, tPtr, tCtr, tPred, 
                                 tPrev, tMi, tPP, xc, xk, xshrunk, rs, stolen, 
                                 batch, nstolen, cand, bsPtr, bsCtr, bsPred, 
                                 bpp, pm, ws, wforce, wa, qsz, qshard, qi, 
                                 qpop, qdef, qclaimed, gc, gk, gtail, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

FB_size(self) == /\ pc[self] = "FB_size"
                 /\ Assert(~InUserRegion(sPtr[self]), 
                           "Failure of assertion at line 491, column 5.")
                 /\ hdrSize' = [hdrSize EXCEPT ![sPtr[self]] = gsz[self]]
                 /\ pc' = [pc EXCEPT ![self] = "FB_link"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrPrev, 
                                 mState, mPtr, mSize, mShard, metaCount, 
                                 mgrActive, mgrTrimming, mgrFree, bins, 
                                 pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, held, stolenOutstanding, 
                                 stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                 xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                 cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                 wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                 qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                 sPred, nxt, asz, ameta, ablk, areserved, da, 
                                 dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                 idsel >>

FB_link(self) == /\ pc[self] = "FB_link"
                 /\ IF FrontierMode = "post_cas_hint"
                       THEN /\ Assert(~InUserRegion(nxt[self]), 
                                      "Failure of assertion at line 495, column 9.")
                            /\ hdrPrev' = [hdrPrev EXCEPT ![nxt[self]] = sPtr[self]]
                       ELSE /\ IF FrontierMode = "packed"
                                  THEN /\ hdrPrev' = [hdrPrev EXCEPT ![sPtr[self]] = sPred[self]]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED hdrPrev
                 /\ IF FrontierMode # "pre_cas_backlink"
                       THEN /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] - 1]
                       ELSE /\ TRUE
                            /\ UNCHANGED guardDepth
                 /\ rvBlk' = [rvBlk EXCEPT ![self] = sPtr[self]]
                 /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                 /\ sPtr' = [sPtr EXCEPT ![self] = Head(stack[self]).sPtr]
                 /\ sCtr' = [sCtr EXCEPT ![self] = Head(stack[self]).sCtr]
                 /\ sPred' = [sPred EXCEPT ![self] = Head(stack[self]).sPred]
                 /\ nxt' = [nxt EXCEPT ![self] = Head(stack[self]).nxt]
                 /\ gsz' = [gsz EXCEPT ![self] = Head(stack[self]).gsz]
                 /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 mState, mPtr, mSize, mShard, metaCount, 
                                 mgrActive, mgrTrimming, mgrFree, bins, 
                                 pending, pendingCount, windowOpen, 
                                 reclaimBusy, irqActive, rvMeta, rvIdx, live, 
                                 held, stolenOutstanding, rm, tPtr, tCtr, 
                                 tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                 stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                 bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                 qi, qpop, qdef, qclaimed, gc, gk, gtail, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

GetEmptyBlock(self) == FB_load(self) \/ FB_check(self) \/ FB_pre(self)
                          \/ FB_cas(self) \/ FB_recheck(self)
                          \/ FB_undo(self) \/ FB_size(self)
                          \/ FB_link(self)

AL_window(self) == /\ pc[self] = "AL_window"
                   /\ /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "MaybeWindow",
                                                               pc        |->  "AL_search",
                                                               ws        |->  ws[self],
                                                               wforce    |->  wforce[self] ] >>
                                                           \o stack[self]]
                      /\ wforce' = [wforce EXCEPT ![self] = FALSE]
                      /\ ws' = [ws EXCEPT ![self] = CpuOf(self)]
                   /\ pc' = [pc EXCEPT ![self] = "MW_count"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, 
                                   xk, xshrunk, rs, stolen, batch, nstolen, 
                                   cand, bsPtr, bsCtr, bsPred, bpp, pm, wa, 
                                   qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                   gk, gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                   ameta, ablk, areserved, da, dmi, dtarget, 
                                   ops, asel, dsel, iops, iasel, idsel >>

AL_search(self) == /\ pc[self] = "AL_search"
                   /\ /\ qshard' = [qshard EXCEPT ![self] = CpuOf(self)]
                      /\ qsz' = [qsz EXCEPT ![self] = asz[self]]
                      /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Search",
                                                               pc        |->  "AL_found",
                                                               qi        |->  qi[self],
                                                               qpop      |->  qpop[self],
                                                               qdef      |->  qdef[self],
                                                               qclaimed  |->  qclaimed[self],
                                                               qsz       |->  qsz[self],
                                                               qshard    |->  qshard[self] ] >>
                                                           \o stack[self]]
                   /\ qi' = [qi EXCEPT ![self] = 1]
                   /\ qpop' = [qpop EXCEPT ![self] = NULL]
                   /\ qdef' = [qdef EXCEPT ![self] = NULL]
                   /\ qclaimed' = [qclaimed EXCEPT ![self] = NULL]
                   /\ pc' = [pc EXCEPT ![self] = "SR_restart"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, 
                                   xk, xshrunk, rs, stolen, batch, nstolen, 
                                   cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                   wforce, wa, gc, gk, gtail, gsz, sPtr, sCtr, 
                                   sPred, nxt, asz, ameta, ablk, areserved, da, 
                                   dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                   idsel >>

AL_found(self) == /\ pc[self] = "AL_found"
                  /\ IF rvMeta[self] # NULL
                        THEN /\ ameta' = [ameta EXCEPT ![self] = rvMeta[self]]
                             /\ ablk' = [ablk EXCEPT ![self] = mPtr[rvMeta[self]]]
                             /\ pc' = [pc EXCEPT ![self] = "AL_fin"]
                             /\ UNCHANGED << stack, gc, gk, gtail >>
                        ELSE /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "GetMetaIndex",
                                                                      pc        |->  "AL_reserved",
                                                                      gc        |->  gc[self],
                                                                      gk        |->  gk[self],
                                                                      gtail     |->  gtail[self] ] >>
                                                                  \o stack[self]]
                             /\ gc' = [gc EXCEPT ![self] = 0]
                             /\ gk' = [gk EXCEPT ![self] = 0]
                             /\ gtail' = [gtail EXCEPT ![self] = 0]
                             /\ pc' = [pc EXCEPT ![self] = "GM_load"]
                             /\ UNCHANGED << ameta, ablk >>
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, 
                                  xk, xshrunk, rs, stolen, batch, nstolen, 
                                  cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                  wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                  qclaimed, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                  areserved, da, dmi, dtarget, ops, asel, dsel, 
                                  iops, iasel, idsel >>

AL_reserved(self) == /\ pc[self] = "AL_reserved"
                     /\ IF rvIdx[self] = NULL
                           THEN /\ pc' = [pc EXCEPT ![self] = "AL_out"]
                                /\ UNCHANGED << stack, gsz, sPtr, sCtr, sPred, 
                                                nxt, areserved >>
                           ELSE /\ areserved' = [areserved EXCEPT ![self] = rvIdx[self]]
                                /\ /\ gsz' = [gsz EXCEPT ![self] = asz[self]]
                                   /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "GetEmptyBlock",
                                                                            pc        |->  "AL_frontier",
                                                                            sPtr      |->  sPtr[self],
                                                                            sCtr      |->  sCtr[self],
                                                                            sPred     |->  sPred[self],
                                                                            nxt       |->  nxt[self],
                                                                            gsz       |->  gsz[self] ] >>
                                                                        \o stack[self]]
                                /\ sPtr' = [sPtr EXCEPT ![self] = 0]
                                /\ sCtr' = [sCtr EXCEPT ![self] = 0]
                                /\ sPred' = [sPred EXCEPT ![self] = NULL]
                                /\ nxt' = [nxt EXCEPT ![self] = 0]
                                /\ pc' = [pc EXCEPT ![self] = "FB_load"]
                     /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                     hdrPrev, mState, mPtr, mSize, mShard, 
                                     metaCount, mgrActive, mgrTrimming, 
                                     mgrFree, bins, pending, pendingCount, 
                                     windowOpen, reclaimBusy, guardDepth, 
                                     irqActive, rvMeta, rvIdx, rvBlk, live, 
                                     held, stolenOutstanding, rm, tPtr, tCtr, 
                                     tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                     rs, stolen, batch, nstolen, cand, bsPtr, 
                                     bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                     qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                     gk, gtail, asz, ameta, ablk, da, dmi, 
                                     dtarget, ops, asel, dsel, iops, iasel, 
                                     idsel >>

AL_frontier(self) == /\ pc[self] = "AL_frontier"
                     /\ IF rvBlk[self] # NULL
                           THEN /\ ablk' = [ablk EXCEPT ![self] = rvBlk[self]]
                                /\ ameta' = [ameta EXCEPT ![self] = areserved[self]]
                                /\ pc' = [pc EXCEPT ![self] = "AL_fin"]
                                /\ UNCHANGED << stack, ws, wforce >>
                           ELSE /\ /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "MaybeWindow",
                                                                            pc        |->  "AL_search2",
                                                                            ws        |->  ws[self],
                                                                            wforce    |->  wforce[self] ] >>
                                                                        \o stack[self]]
                                   /\ wforce' = [wforce EXCEPT ![self] = TRUE]
                                   /\ ws' = [ws EXCEPT ![self] = CpuOf(self)]
                                /\ pc' = [pc EXCEPT ![self] = "MW_count"]
                                /\ UNCHANGED << ameta, ablk >>
                     /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                     hdrPrev, mState, mPtr, mSize, mShard, 
                                     metaCount, mgrActive, mgrTrimming, 
                                     mgrFree, bins, pending, pendingCount, 
                                     windowOpen, reclaimBusy, guardDepth, 
                                     irqActive, rvMeta, rvIdx, rvBlk, live, 
                                     held, stolenOutstanding, rm, tPtr, tCtr, 
                                     tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                     rs, stolen, batch, nstolen, cand, bsPtr, 
                                     bsCtr, bsPred, bpp, pm, wa, qsz, qshard, 
                                     qi, qpop, qdef, qclaimed, gc, gk, gtail, 
                                     gsz, sPtr, sCtr, sPred, nxt, asz, 
                                     areserved, da, dmi, dtarget, ops, asel, 
                                     dsel, iops, iasel, idsel >>

AL_search2(self) == /\ pc[self] = "AL_search2"
                    /\ /\ qshard' = [qshard EXCEPT ![self] = CpuOf(self)]
                       /\ qsz' = [qsz EXCEPT ![self] = asz[self]]
                       /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Search",
                                                                pc        |->  "AL_found2",
                                                                qi        |->  qi[self],
                                                                qpop      |->  qpop[self],
                                                                qdef      |->  qdef[self],
                                                                qclaimed  |->  qclaimed[self],
                                                                qsz       |->  qsz[self],
                                                                qshard    |->  qshard[self] ] >>
                                                            \o stack[self]]
                    /\ qi' = [qi EXCEPT ![self] = 1]
                    /\ qpop' = [qpop EXCEPT ![self] = NULL]
                    /\ qdef' = [qdef EXCEPT ![self] = NULL]
                    /\ qclaimed' = [qclaimed EXCEPT ![self] = NULL]
                    /\ pc' = [pc EXCEPT ![self] = "SR_restart"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrActive, mgrTrimming, mgrFree, 
                                    bins, pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, rm, tPtr, tCtr, tPred, 
                                    tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                    stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                    bsPred, bpp, pm, ws, wforce, wa, gc, gk, 
                                    gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                    ameta, ablk, areserved, da, dmi, dtarget, 
                                    ops, asel, dsel, iops, iasel, idsel >>

AL_found2(self) == /\ pc[self] = "AL_found2"
                   /\ IF rvMeta[self] # NULL
                         THEN /\ pc' = [pc EXCEPT ![self] = "AL_reuse"]
                              /\ UNCHANGED << stack, wa >>
                         ELSE /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "RunWindowAll",
                                                                       pc        |->  "AL_search3",
                                                                       wa        |->  wa[self] ] >>
                                                                   \o stack[self]]
                              /\ wa' = [wa EXCEPT ![self] = 1]
                              /\ pc' = [pc EXCEPT ![self] = "WA_loop"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, 
                                   xk, xshrunk, rs, stolen, batch, nstolen, 
                                   cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                   wforce, qsz, qshard, qi, qpop, qdef, 
                                   qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                   sPred, nxt, asz, ameta, ablk, areserved, da, 
                                   dmi, dtarget, ops, asel, dsel, iops, iasel, 
                                   idsel >>

AL_search3(self) == /\ pc[self] = "AL_search3"
                    /\ /\ qshard' = [qshard EXCEPT ![self] = CpuOf(self)]
                       /\ qsz' = [qsz EXCEPT ![self] = asz[self]]
                       /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Search",
                                                                pc        |->  "AL_found3",
                                                                qi        |->  qi[self],
                                                                qpop      |->  qpop[self],
                                                                qdef      |->  qdef[self],
                                                                qclaimed  |->  qclaimed[self],
                                                                qsz       |->  qsz[self],
                                                                qshard    |->  qshard[self] ] >>
                                                            \o stack[self]]
                    /\ qi' = [qi EXCEPT ![self] = 1]
                    /\ qpop' = [qpop EXCEPT ![self] = NULL]
                    /\ qdef' = [qdef EXCEPT ![self] = NULL]
                    /\ qclaimed' = [qclaimed EXCEPT ![self] = NULL]
                    /\ pc' = [pc EXCEPT ![self] = "SR_restart"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mState, mPtr, mSize, mShard, 
                                    metaCount, mgrActive, mgrTrimming, mgrFree, 
                                    bins, pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, rm, tPtr, tCtr, tPred, 
                                    tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                    stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                    bsPred, bpp, pm, ws, wforce, wa, gc, gk, 
                                    gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                    ameta, ablk, areserved, da, dmi, dtarget, 
                                    ops, asel, dsel, iops, iasel, idsel >>

AL_found3(self) == /\ pc[self] = "AL_found3"
                   /\ IF rvMeta[self] # NULL
                         THEN /\ pc' = [pc EXCEPT ![self] = "AL_reuse"]
                              /\ UNCHANGED << stack, rm >>
                         ELSE /\ /\ rm' = [rm EXCEPT ![self] = areserved[self]]
                                 /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "RecycleMetadata",
                                                                          pc        |->  "AL_out",
                                                                          rm        |->  rm[self] ] >>
                                                                      \o stack[self]]
                              /\ pc' = [pc EXCEPT ![self] = "RM_state"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                   xshrunk, rs, stolen, batch, nstolen, cand, 
                                   bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                   wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                   gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                   asz, ameta, ablk, areserved, da, dmi, 
                                   dtarget, ops, asel, dsel, iops, iasel, 
                                   idsel >>

AL_reuse(self) == /\ pc[self] = "AL_reuse"
                  /\ ameta' = [ameta EXCEPT ![self] = rvMeta[self]]
                  /\ ablk' = [ablk EXCEPT ![self] = mPtr[rvMeta[self]]]
                  /\ /\ rm' = [rm EXCEPT ![self] = areserved[self]]
                     /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "RecycleMetadata",
                                                              pc        |->  "AL_fin",
                                                              rm        |->  rm[self] ] >>
                                                          \o stack[self]]
                  /\ pc' = [pc EXCEPT ![self] = "RM_state"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pending, pendingCount, windowOpen, 
                                  reclaimBusy, guardDepth, irqActive, rvMeta, 
                                  rvIdx, rvBlk, live, held, stolenOutstanding, 
                                  tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                  xshrunk, rs, stolen, batch, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                  wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                  gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                  asz, areserved, da, dmi, dtarget, ops, asel, 
                                  dsel, iops, iasel, idsel >>

AL_fin(self) == /\ pc[self] = "AL_fin"
                /\ Assert(~InUserRegion(ablk[self]), 
                          "Failure of assertion at line 536, column 5.")
                /\ Assert(hdrSize[ablk[self]] = asz[self], 
                          "Failure of assertion at line 537, column 5.")
                /\ hdrMeta' = [hdrMeta EXCEPT ![ablk[self]] = ameta[self]]
                /\ pc' = [pc EXCEPT ![self] = "AL_inuse"]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrSize, hdrPrev, mState, 
                                mPtr, mSize, mShard, metaCount, mgrActive, 
                                mgrTrimming, mgrFree, bins, pending, 
                                pendingCount, windowOpen, reclaimBusy, 
                                guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                live, held, stolenOutstanding, stack, rm, tPtr, 
                                tCtr, tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                rs, stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                areserved, da, dmi, dtarget, ops, asel, dsel, 
                                iops, iasel, idsel >>

AL_inuse(self) == /\ pc[self] = "AL_inuse"
                  /\ mState' = [mState EXCEPT ![ameta[self]] = "IN_USE"]
                  /\ mPtr' = [mPtr EXCEPT ![ameta[self]] = ablk[self]]
                  /\ pc' = [pc EXCEPT ![self] = "AL_fields"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mSize, mShard, metaCount, mgrActive, 
                                  mgrTrimming, mgrFree, bins, pending, 
                                  pendingCount, windowOpen, reclaimBusy, 
                                  guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                  live, held, stolenOutstanding, stack, rm, 
                                  tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                  xshrunk, rs, stolen, batch, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                  wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                  gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                  asz, ameta, ablk, areserved, da, dmi, 
                                  dtarget, ops, asel, dsel, iops, iasel, idsel >>

AL_fields(self) == /\ pc[self] = "AL_fields"
                   /\ mSize' = [mSize EXCEPT ![ameta[self]] = hdrSize[ablk[self]]]
                   /\ mShard' = [mShard EXCEPT ![ameta[self]] = CpuOf(self)]
                   /\ pc' = [pc EXCEPT ![self] = "AL_live"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, metaCount, mgrActive, 
                                   mgrTrimming, mgrFree, bins, pending, 
                                   pendingCount, windowOpen, reclaimBusy, 
                                   guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                   live, held, stolenOutstanding, stack, rm, 
                                   tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                   xshrunk, rs, stolen, batch, nstolen, cand, 
                                   bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                   wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                   gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                   asz, ameta, ablk, areserved, da, dmi, 
                                   dtarget, ops, asel, dsel, iops, iasel, 
                                   idsel >>

AL_live(self) == /\ pc[self] = "AL_live"
                 /\ live' = (live \cup {[start |-> ablk[self], size |-> asz[self]]})
                 /\ held' = [held EXCEPT ![self] = held[self] \cup {[start |-> ablk[self], size |-> asz[self]]}]
                 /\ pc' = [pc EXCEPT ![self] = "AL_out"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, stolenOutstanding, stack, rm, 
                                 tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

AL_out(self) == /\ pc[self] = "AL_out"
                /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                /\ ameta' = [ameta EXCEPT ![self] = Head(stack[self]).ameta]
                /\ ablk' = [ablk EXCEPT ![self] = Head(stack[self]).ablk]
                /\ areserved' = [areserved EXCEPT ![self] = Head(stack[self]).areserved]
                /\ asz' = [asz EXCEPT ![self] = Head(stack[self]).asz]
                /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                xshrunk, rs, stolen, batch, nstolen, cand, 
                                bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                gtail, gsz, sPtr, sCtr, sPred, nxt, da, dmi, 
                                dtarget, ops, asel, dsel, iops, iasel, idsel >>

Allocate(self) == AL_window(self) \/ AL_search(self) \/ AL_found(self)
                     \/ AL_reserved(self) \/ AL_frontier(self)
                     \/ AL_search2(self) \/ AL_found2(self)
                     \/ AL_search3(self) \/ AL_found3(self)
                     \/ AL_reuse(self) \/ AL_fin(self) \/ AL_inuse(self)
                     \/ AL_fields(self) \/ AL_live(self) \/ AL_out(self)

DA_user(self) == /\ pc[self] = "DA_user"
                 /\ live' = live \ {da[self]}
                 /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] + 1]
                 /\ pc' = [pc EXCEPT ![self] = "DA_idx"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, irqActive, rvMeta, rvIdx, rvBlk, 
                                 held, stolenOutstanding, stack, rm, tPtr, 
                                 tCtr, tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                 rs, stolen, batch, nstolen, cand, bsPtr, 
                                 bsCtr, bsPred, bpp, pm, ws, wforce, wa, qsz, 
                                 qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

DA_idx(self) == /\ pc[self] = "DA_idx"
                /\ dmi' = [dmi EXCEPT ![self] = hdrMeta[da[self].start]]
                /\ pc' = [pc EXCEPT ![self] = "DA_count_check"]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                stack, rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, 
                                xc, xk, xshrunk, rs, stolen, batch, nstolen, 
                                cand, bsPtr, bsCtr, bsPred, bpp, pm, ws, 
                                wforce, wa, qsz, qshard, qi, qpop, qdef, 
                                qclaimed, gc, gk, gtail, gsz, sPtr, sCtr, 
                                sPred, nxt, asz, ameta, ablk, areserved, da, 
                                dtarget, ops, asel, dsel, iops, iasel, idsel >>

DA_count_check(self) == /\ pc[self] = "DA_count_check"
                        /\ Assert(dmi[self] # NULL /\ dmi[self] < metaCount, 
                                  "Failure of assertion at line 562, column 5.")
                        /\ pc' = [pc EXCEPT ![self] = "DA_lock"]
                        /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                        hdrPrev, mState, mPtr, mSize, mShard, 
                                        metaCount, mgrActive, mgrTrimming, 
                                        mgrFree, bins, pending, pendingCount, 
                                        windowOpen, reclaimBusy, guardDepth, 
                                        irqActive, rvMeta, rvIdx, rvBlk, live, 
                                        held, stolenOutstanding, stack, rm, 
                                        tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, 
                                        xk, xshrunk, rs, stolen, batch, 
                                        nstolen, cand, bsPtr, bsCtr, bsPred, 
                                        bpp, pm, ws, wforce, wa, qsz, qshard, 
                                        qi, qpop, qdef, qclaimed, gc, gk, 
                                        gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                        asz, ameta, ablk, areserved, da, dmi, 
                                        dtarget, ops, asel, dsel, iops, iasel, 
                                        idsel >>

DA_lock(self) == /\ pc[self] = "DA_lock"
                 /\ Assert(mState[dmi[self]] = "IN_USE" /\ mPtr[dmi[self]] = da[self].start, 
                           "Failure of assertion at line 564, column 5.")
                 /\ mState' = [mState EXCEPT ![dmi[self]] = "LOCKED"]
                 /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] - 1]
                 /\ pc' = [pc EXCEPT ![self] = "DA_pending"]
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mPtr, mSize, mShard, metaCount, 
                                 mgrActive, mgrTrimming, mgrFree, bins, 
                                 pending, pendingCount, windowOpen, 
                                 reclaimBusy, irqActive, rvMeta, rvIdx, rvBlk, 
                                 live, held, stolenOutstanding, stack, rm, 
                                 tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                 ameta, ablk, areserved, da, dmi, dtarget, ops, 
                                 asel, dsel, iops, iasel, idsel >>

DA_pending(self) == /\ pc[self] = "DA_pending"
                    /\ mState' = [mState EXCEPT ![dmi[self]] = "DEALLOCATED_PENDING"]
                    /\ dtarget' = [dtarget EXCEPT ![self] = mShard[dmi[self]]]
                    /\ IF PendingCountFirst
                          THEN /\ pc' = [pc EXCEPT ![self] = "DA_count1"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "DA_push0"]
                    /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                    hdrPrev, mPtr, mSize, mShard, metaCount, 
                                    mgrActive, mgrTrimming, mgrFree, bins, 
                                    pending, pendingCount, windowOpen, 
                                    reclaimBusy, guardDepth, irqActive, rvMeta, 
                                    rvIdx, rvBlk, live, held, 
                                    stolenOutstanding, stack, rm, tPtr, tCtr, 
                                    tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                    rs, stolen, batch, nstolen, cand, bsPtr, 
                                    bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                    qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                    gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                    asz, ameta, ablk, areserved, da, dmi, ops, 
                                    asel, dsel, iops, iasel, idsel >>

DA_push0(self) == /\ pc[self] = "DA_push0"
                  /\ Assert(~InAnyList(dmi[self]), 
                            "Failure of assertion at line 572, column 5.")
                  /\ pending' = [pending EXCEPT ![dtarget[self]] = <<dmi[self]>> \o pending[dtarget[self]]]
                  /\ pc' = [pc EXCEPT ![self] = "DA_count0"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pendingCount, windowOpen, reclaimBusy, 
                                  guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                  live, held, stolenOutstanding, stack, rm, 
                                  tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                  xshrunk, rs, stolen, batch, nstolen, cand, 
                                  bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                  wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                  gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                  asz, ameta, ablk, areserved, da, dmi, 
                                  dtarget, ops, asel, dsel, iops, iasel, idsel >>

DA_count0(self) == /\ pc[self] = "DA_count0"
                   /\ pendingCount' = [pendingCount EXCEPT ![dtarget[self]] = pendingCount[dtarget[self]] + 1]
                   /\ pc' = [pc EXCEPT ![self] = "DA_window"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, windowOpen, reclaimBusy, 
                                   guardDepth, irqActive, rvMeta, rvIdx, rvBlk, 
                                   live, held, stolenOutstanding, stack, rm, 
                                   tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                   xshrunk, rs, stolen, batch, nstolen, cand, 
                                   bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, 
                                   wa, qsz, qshard, qi, qpop, qdef, qclaimed, 
                                   gc, gk, gtail, gsz, sPtr, sCtr, sPred, nxt, 
                                   asz, ameta, ablk, areserved, da, dmi, 
                                   dtarget, ops, asel, dsel, iops, iasel, 
                                   idsel >>

DA_count1(self) == /\ pc[self] = "DA_count1"
                   /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] + 1]
                   /\ pendingCount' = [pendingCount EXCEPT ![dtarget[self]] = pendingCount[dtarget[self]] + 1]
                   /\ pc' = [pc EXCEPT ![self] = "DA_push1"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, windowOpen, reclaimBusy, 
                                   irqActive, rvMeta, rvIdx, rvBlk, live, held, 
                                   stolenOutstanding, stack, rm, tPtr, tCtr, 
                                   tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                   stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                   bsPred, bpp, pm, ws, wforce, wa, qsz, 
                                   qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                   gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                   ameta, ablk, areserved, da, dmi, dtarget, 
                                   ops, asel, dsel, iops, iasel, idsel >>

DA_push1(self) == /\ pc[self] = "DA_push1"
                  /\ Assert(~InAnyList(dmi[self]), 
                            "Failure of assertion at line 581, column 5.")
                  /\ pending' = [pending EXCEPT ![dtarget[self]] = <<dmi[self]>> \o pending[dtarget[self]]]
                  /\ guardDepth' = [guardDepth EXCEPT ![CpuOf(self)] = guardDepth[CpuOf(self)] - 1]
                  /\ pc' = [pc EXCEPT ![self] = "DA_window"]
                  /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                  hdrPrev, mState, mPtr, mSize, mShard, 
                                  metaCount, mgrActive, mgrTrimming, mgrFree, 
                                  bins, pendingCount, windowOpen, reclaimBusy, 
                                  irqActive, rvMeta, rvIdx, rvBlk, live, held, 
                                  stolenOutstanding, stack, rm, tPtr, tCtr, 
                                  tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                  stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                  bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                  qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                  sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                  areserved, da, dmi, dtarget, ops, asel, dsel, 
                                  iops, iasel, idsel >>

DA_window(self) == /\ pc[self] = "DA_window"
                   /\ /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "MaybeWindow",
                                                               pc        |->  "DA_ret",
                                                               ws        |->  ws[self],
                                                               wforce    |->  wforce[self] ] >>
                                                           \o stack[self]]
                      /\ wforce' = [wforce EXCEPT ![self] = FALSE]
                      /\ ws' = [ws EXCEPT ![self] = dtarget[self]]
                   /\ pc' = [pc EXCEPT ![self] = "MW_count"]
                   /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                   hdrPrev, mState, mPtr, mSize, mShard, 
                                   metaCount, mgrActive, mgrTrimming, mgrFree, 
                                   bins, pending, pendingCount, windowOpen, 
                                   reclaimBusy, guardDepth, irqActive, rvMeta, 
                                   rvIdx, rvBlk, live, held, stolenOutstanding, 
                                   rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, 
                                   xk, xshrunk, rs, stolen, batch, nstolen, 
                                   cand, bsPtr, bsCtr, bsPred, bpp, pm, wa, 
                                   qsz, qshard, qi, qpop, qdef, qclaimed, gc, 
                                   gk, gtail, gsz, sPtr, sCtr, sPred, nxt, asz, 
                                   ameta, ablk, areserved, da, dmi, dtarget, 
                                   ops, asel, dsel, iops, iasel, idsel >>

DA_ret(self) == /\ pc[self] = "DA_ret"
                /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                /\ dmi' = [dmi EXCEPT ![self] = Head(stack[self]).dmi]
                /\ dtarget' = [dtarget EXCEPT ![self] = Head(stack[self]).dtarget]
                /\ da' = [da EXCEPT ![self] = Head(stack[self]).da]
                /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, irqActive, rvMeta, 
                                rvIdx, rvBlk, live, held, stolenOutstanding, 
                                rm, tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                xshrunk, rs, stolen, batch, nstolen, cand, 
                                bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                gtail, gsz, sPtr, sCtr, sPred, nxt, asz, ameta, 
                                ablk, areserved, ops, asel, dsel, iops, iasel, 
                                idsel >>

Deallocate(self) == DA_user(self) \/ DA_idx(self) \/ DA_count_check(self)
                       \/ DA_lock(self) \/ DA_pending(self)
                       \/ DA_push0(self) \/ DA_count0(self)
                       \/ DA_count1(self) \/ DA_push1(self)
                       \/ DA_window(self) \/ DA_ret(self)

MN_loop(self) == /\ pc[self] = "MN_loop"
                 /\ IF ops[self] < OpsPerThread[self]
                       THEN /\ ops' = [ops EXCEPT ![self] = ops[self] + 1]
                            /\ \/ /\ \E z \in Sizes:
                                       asel' = [asel EXCEPT ![self] = z]
                                  /\ /\ asz' = [asz EXCEPT ![self] = asel'[self]]
                                     /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Allocate",
                                                                              pc        |->  "MN_loop",
                                                                              ameta     |->  ameta[self],
                                                                              ablk      |->  ablk[self],
                                                                              areserved |->  areserved[self],
                                                                              asz       |->  asz[self] ] >>
                                                                          \o stack[self]]
                                  /\ ameta' = [ameta EXCEPT ![self] = NULL]
                                  /\ ablk' = [ablk EXCEPT ![self] = NULL]
                                  /\ areserved' = [areserved EXCEPT ![self] = NULL]
                                  /\ pc' = [pc EXCEPT ![self] = "AL_window"]
                                  /\ UNCHANGED <<held, da, dmi, dtarget, dsel>>
                               \/ /\ held[self] # {}
                                  /\ \E a \in held[self]:
                                       /\ dsel' = [dsel EXCEPT ![self] = a]
                                       /\ held' = [held EXCEPT ![self] = held[self] \ {a}]
                                  /\ /\ da' = [da EXCEPT ![self] = dsel'[self]]
                                     /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Deallocate",
                                                                              pc        |->  "MN_loop",
                                                                              dmi       |->  dmi[self],
                                                                              dtarget   |->  dtarget[self],
                                                                              da        |->  da[self] ] >>
                                                                          \o stack[self]]
                                  /\ dmi' = [dmi EXCEPT ![self] = NULL]
                                  /\ dtarget' = [dtarget EXCEPT ![self] = 1]
                                  /\ pc' = [pc EXCEPT ![self] = "DA_user"]
                                  /\ UNCHANGED <<asz, ameta, ablk, areserved, asel>>
                       ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                            /\ UNCHANGED << held, stack, asz, ameta, ablk, 
                                            areserved, da, dmi, dtarget, ops, 
                                            asel, dsel >>
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, irqActive, rvMeta, 
                                 rvIdx, rvBlk, live, stolenOutstanding, rm, 
                                 tPtr, tCtr, tPred, tPrev, tMi, tPP, xc, xk, 
                                 xshrunk, rs, stolen, batch, nstolen, cand, 
                                 bsPtr, bsCtr, bsPred, bpp, pm, ws, wforce, wa, 
                                 qsz, qshard, qi, qpop, qdef, qclaimed, gc, gk, 
                                 gtail, gsz, sPtr, sCtr, sPred, nxt, iops, 
                                 iasel, idsel >>

Main(self) == MN_loop(self)

IQ_loop(self) == /\ pc[self] = "IQ_loop"
                 /\ IF iops[self] < MaxIrqOps
                       THEN /\ guardDepth[CpuOf(self)] = 0
                            /\ irqActive' = [irqActive EXCEPT ![CpuOf(self)] = TRUE]
                            /\ iops' = [iops EXCEPT ![self] = iops[self] + 1]
                            /\ \/ /\ \E z \in Sizes:
                                       iasel' = [iasel EXCEPT ![self] = z]
                                  /\ /\ asz' = [asz EXCEPT ![self] = iasel'[self]]
                                     /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Allocate",
                                                                              pc        |->  "IQ_end",
                                                                              ameta     |->  ameta[self],
                                                                              ablk      |->  ablk[self],
                                                                              areserved |->  areserved[self],
                                                                              asz       |->  asz[self] ] >>
                                                                          \o stack[self]]
                                  /\ ameta' = [ameta EXCEPT ![self] = NULL]
                                  /\ ablk' = [ablk EXCEPT ![self] = NULL]
                                  /\ areserved' = [areserved EXCEPT ![self] = NULL]
                                  /\ pc' = [pc EXCEPT ![self] = "AL_window"]
                                  /\ UNCHANGED <<held, da, dmi, dtarget, idsel>>
                               \/ /\ held[self] # {}
                                  /\ \E a \in held[self]:
                                       /\ idsel' = [idsel EXCEPT ![self] = a]
                                       /\ held' = [held EXCEPT ![self] = held[self] \ {a}]
                                  /\ /\ da' = [da EXCEPT ![self] = idsel'[self]]
                                     /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Deallocate",
                                                                              pc        |->  "IQ_end",
                                                                              dmi       |->  dmi[self],
                                                                              dtarget   |->  dtarget[self],
                                                                              da        |->  da[self] ] >>
                                                                          \o stack[self]]
                                  /\ dmi' = [dmi EXCEPT ![self] = NULL]
                                  /\ dtarget' = [dtarget EXCEPT ![self] = 1]
                                  /\ pc' = [pc EXCEPT ![self] = "DA_user"]
                                  /\ UNCHANGED <<asz, ameta, ablk, areserved, iasel>>
                       ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                            /\ UNCHANGED << irqActive, held, stack, asz, ameta, 
                                            ablk, areserved, da, dmi, dtarget, 
                                            iops, iasel, idsel >>
                 /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                 hdrPrev, mState, mPtr, mSize, mShard, 
                                 metaCount, mgrActive, mgrTrimming, mgrFree, 
                                 bins, pending, pendingCount, windowOpen, 
                                 reclaimBusy, guardDepth, rvMeta, rvIdx, rvBlk, 
                                 live, stolenOutstanding, rm, tPtr, tCtr, 
                                 tPred, tPrev, tMi, tPP, xc, xk, xshrunk, rs, 
                                 stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                 bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                 qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                 sPtr, sCtr, sPred, nxt, ops, asel, dsel >>

IQ_end(self) == /\ pc[self] = "IQ_end"
                /\ irqActive' = [irqActive EXCEPT ![CpuOf(self)] = FALSE]
                /\ pc' = [pc EXCEPT ![self] = "IQ_loop"]
                /\ UNCHANGED << fwPtr, fwCtr, fwPred, hdrMeta, hdrSize, 
                                hdrPrev, mState, mPtr, mSize, mShard, 
                                metaCount, mgrActive, mgrTrimming, mgrFree, 
                                bins, pending, pendingCount, windowOpen, 
                                reclaimBusy, guardDepth, rvMeta, rvIdx, rvBlk, 
                                live, held, stolenOutstanding, stack, rm, tPtr, 
                                tCtr, tPred, tPrev, tMi, tPP, xc, xk, xshrunk, 
                                rs, stolen, batch, nstolen, cand, bsPtr, bsCtr, 
                                bsPred, bpp, pm, ws, wforce, wa, qsz, qshard, 
                                qi, qpop, qdef, qclaimed, gc, gk, gtail, gsz, 
                                sPtr, sCtr, sPred, nxt, asz, ameta, ablk, 
                                areserved, da, dmi, dtarget, ops, asel, dsel, 
                                iops, iasel, idsel >>

Irq(self) == IQ_loop(self) \/ IQ_end(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in ProcSet:  \/ RecycleMetadata(self) \/ TryReclaim(self)
                               \/ Trim(self) \/ RunWindow(self)
                               \/ MaybeWindow(self) \/ RunWindowAll(self)
                               \/ Search(self) \/ GetMetaIndex(self)
                               \/ GetEmptyBlock(self) \/ Allocate(self)
                               \/ Deallocate(self))
           \/ (\E self \in MainProcs: Main(self))
           \/ (\E self \in IrqProcs: Irq(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

-----------------------------------------------------------------------------
(* Interrupt semantics: while a CPU's handler is active, that CPU's main     *)
(* thread does not run.  The handler can only start when the CPU is outside *)
(* every interrupt_guard (IQ_loop's await).                                  *)

CanRun(self) == IF self \in MainProcs THEN ~irqActive[self] ELSE TRUE

StepOf(self) ==
    \/ RecycleMetadata(self) \/ TryReclaim(self) \/ Trim(self) \/ RunWindow(self)
    \/ MaybeWindow(self) \/ RunWindowAll(self) \/ Search(self) \/ GetMetaIndex(self)
    \/ GetEmptyBlock(self) \/ Allocate(self) \/ Deallocate(self)
    \/ (self \in MainProcs /\ Main(self))
    \/ (self \in IrqProcs /\ Irq(self))

NextI == (\E self \in ProcSet : CanRun(self) /\ StepOf(self)) \/ Terminating

SpecI == Init /\ [][NextI]_vars

\* Weak fairness for every process that can run: no spin or undo loop (FB_undo, GM_bundo, GM_breserve, the
\*   reclaim and trim retries) may keep a process from finishing.  Checked with PROPERTY Termination.
SpecIFair == SpecI /\ \A p \in ProcSet : WF_vars(CanRun(p) /\ StepOf(p))

-----------------------------------------------------------------------------
(* Invariants                                                                *)

Disjoint(a, b) == a.start + a.size <= b.start \/ b.start + b.size <= a.start

\* Callers never get overlapping memory, and nothing they own lies beyond the frontier.
NoOverlappingAllocations == \A a, b \in live : a # b => Disjoint(a, b)
LiveBelowFrontier == \A a \in live : a.start + a.size <= fwPtr

\* Blocks the allocator still accounts for are disjoint and below the frontier.
TrackedStates == {"IN_USE", "LOCKED", "DEALLOCATED_PENDING", "AVAILABLE"}
TrackedBlock(m) == [start |-> mPtr[m], size |-> hdrSize[mPtr[m]]]
TrackedBlocksDisjoint ==
    \A m, n \in Metas : (m # n /\ mState[m] \in TrackedStates /\ mState[n] \in TrackedStates)
                            => Disjoint(TrackedBlock(m), TrackedBlock(n))
TrackedBlocksBelowFrontier ==
    \A m \in Metas : mState[m] \in TrackedStates => mPtr[m] + hdrSize[mPtr[m]] <= fwPtr

\* Every list node is in at most one list, and its state matches the list.
ListsDisjoint ==
    /\ \A z1, z2 \in Sizes, c1, c2 \in CPUs : <<z1, c1>> # <<z2, c2>> => bins[z1][c1] \cap bins[z2][c2] = {}
    /\ \A c1, c2 \in CPUs : c1 # c2 => PendSet(c1) \cap PendSet(c2) = {}
    /\ \A c \in CPUs : Cardinality(PendSet(c)) = Len(pending[c])
    /\ \A k1, k2 \in Managers : k1 # k2 => mgrFree[k1] \cap mgrFree[k2] = {}
    /\ \A m \in Metas : ~(InBins(m) /\ InPending(m)) /\ ~(InBins(m) /\ InMgrFree(m)) /\ ~(InPending(m) /\ InMgrFree(m))
ListStatesConsistent ==
    /\ \A k \in Managers : \A m \in mgrFree[k] : mState[m] = "METADATA_AVAILABLE" /\ ManagerOf(m) = k
    /\ \A c \in CPUs : \A m \in PendSet(c) : mState[m] = "DEALLOCATED_PENDING"
    /\ \A z \in Sizes, c \in CPUs : \A m \in bins[z][c] :
           mState[m] \in {"AVAILABLE", "FRONTIER_RECLAIM_IN_PROGRESS", "FRONTIER_RECLAIMED"} /\ mSize[m] = z

\* Records beyond the metadata count are not referenced anywhere.
NothingBeyondMetaCount ==
    \A m \in Metas : m >= metaCount => (mState[m] \in {"INVALID", "METADATA_AVAILABLE"} /\ ~InBins(m) /\ ~InPending(m))

\* N5: once no deallocation is between its push and its count update for a shard,
\*   the count equals the entries queued plus those stolen but not yet subtracted.
DeallocInFlight(c) ==
    \E p \in ProcSet : pc[p] \in {"DA_push0", "DA_count0", "DA_count1", "DA_push1"} /\ dtarget[p] = c
PendingCountExact ==
    \A c \in CPUs : ~DeallocInFlight(c) => pendingCount[c] = Len(pending[c]) + stolenOutstanding[c]

ActiveSlotsNonNegative == \A k \in Managers : mgrActive[k] >= 0

\* Frontier/metadata boundary (record i is at unit MS - i).  With FixedBoundary a claim may overlap
\*   the other region between its CAS and its re-check/undo, so the region check holds at rest.
BoundaryInFlight == \E p \in ProcSet : pc[p] \in {"FB_recheck", "FB_undo", "GM_brecheck", "GM_bundo"}
FrontierBelowMetadata == ~BoundaryInFlight => fwPtr <= MS - metaCount + 1

\* No block the allocator handed out or still tracks covers a metadata record that is in use.
RecordUnit(m) == MS - m
Covers(b, u) == b.start <= u /\ u < b.start + b.size
NoBlockOverRecord ==
    \A m \in Metas : mState[m] # "INVALID" =>
        /\ \A a \in live : ~Covers(a, RecordUnit(m))
        /\ \A n \in Metas : mState[n] \in TrackedStates => ~Covers(TrackedBlock(n), RecordUnit(m))

Safety ==
    /\ FrontierBelowMetadata
    /\ NoBlockOverRecord
    /\ NoOverlappingAllocations
    /\ LiveBelowFrontier
    /\ TrackedBlocksDisjoint
    /\ TrackedBlocksBelowFrontier
    /\ ListsDisjoint
    /\ ListStatesConsistent
    /\ NothingBeyondMetaCount
    /\ PendingCountExact
    /\ ActiveSlotsNonNegative
=============================================================================
