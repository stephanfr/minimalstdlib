# TLA+ model of `lockfree_single_arena_resource`

`LockfreeSingleArena.tla` is a PlusCal model of `include/__memory_resource/lockfree_single_arena_resource.h`. Each label in the model corresponds to one shared-memory access in the C++ code: an atomic load, store, CAS or exchange. Comments next to each label name the C++ function it models.

`boundary/` holds two small models of just the frontier/metadata boundary: checking an overlap there needs five concurrent allocations, which is beyond an exhaustive run of this model.

## What is modelled

| C++ | Model |
|-----|-------|
| `next_empty_memory_block_` (block pointer and its predecessor, packed) | `fwPtr`, `fwPred` |
| `block_header` (`metadata_index_`, `size_including_header_`, `previous_block_`) at every 64-byte unit | `hdrMeta`, `hdrSize`, `hdrPrev` |
| `block_metadata::block_state_` (pointer + state), `total_size_`, `original_shard_` | `mState`, `mPtr`, `mSize`, `mShard` |
| `current_metadata_record_count_` | `metaCount` |
| `metadata_block_manager` (`state_and_active_count_`, `local_free_head_`) | `mgrActive`, `mgrTrimming`, `mgrFree` |
| `free_block_bins_` | `bins[size][shard]` |
| `pending_shards_[s]` (`head_`, `count_`, `maintenance_window_open_`) | `pending`, `pendingCount`, `windowOpen` |
| `frontier_reclaim_in_progress_` | `reclaimBusy` |
| `interrupt_guard` | `guardDepth[cpu]` |

Procedures: `Allocate` (`do_allocate`), `Deallocate` (`do_try_deallocate`), `Search` (`search_for_deallocated_block` + `unguarded_try_claim_popped_block`), `GetMetaIndex` (`get_next_metadata_record_index`), `GetEmptyBlock` (`get_next_empty_memory_block`), `MaybeWindow`, `RunWindow` (`run_maintenance_window`, with `reclaim_pending_frontier_batch` and `process_pending_batch` inline), `RunWindowAll`, `TryReclaim` (`try_reclaim_frontier_blocks`), `Trim` (`trim_metadata_records`), `RecycleMetadata`.

**Threads and interrupts.** There is one `Main` process per CPU, and each CPU is its own shard. `Main` process `i` makes `OpsPerThread[i]` calls, each one an allocate or a free of something it holds. With `UseInterrupts = TRUE`, an `Irq` process per CPU can start only while that CPU is outside every `interrupt_guard`. While it runs, that CPU's `Main` process makes no progress (`CanRun` in `NextI`).

## Model parameters

`InitialBacklinks` sets the initial `previous_block_` contents. The arena buffer is not zeroed, so it can hold anything, including a back-link left over from an earlier layout:
- `CleanArena`: all null.
- `StaleBacklinkAt5`: unit 5 points at unit 2. This reaches, in a 1+3-operation run, the stale back-link that otherwise needs at least 6 operations per thread.

`OpsPerThread` is assigned with `<-` from `Ops_1_1`, `Ops_2_1`, `Ops_2_2`, `Ops_1_3`, `Ops_3`, `Ops_2_2_2`, `Ops_1_4` or `Ops_1_1_1_1_1`, because `.cfg` files can't hold tuples.

## Properties checked

`Safety` (invariant):
- `NoOverlappingAllocations`, `LiveBelowFrontier`: callers never get overlapping memory, and nothing they own lies beyond the frontier.
- `TrackedBlocksDisjoint`, `TrackedBlocksBelowFrontier`: the same for blocks the allocator still tracks (`IN_USE`, `LOCKED`, `DEALLOCATED_PENDING`, `AVAILABLE`).
- `ListsDisjoint`, `ListStatesConsistent`: a metadata record is in at most one list, and its state matches that list.
- `NothingBeyondMetaCount`: a trimmed record is never still referenced.
- `PendingCountExact`: with no free between its count update and its push, `count_` equals the queued entries plus the stolen ones not yet subtracted.
- `ActiveSlotsNonNegative`.
- `FrontierBelowMetadata`: the block region never reaches a metadata record. A claim can overlap the other region between its CAS and its re-check or undo, so this is checked whenever no process is in that window.
- `NoBlockOverRecord`: no block handed out or still tracked covers a metadata record that is in use, at any time.

Assertions, reported by TLC as `Failure of assertion`:
- the allocator never reads or writes a block header that lies inside a caller's live allocation;
- a free of a valid allocation is never rejected;
- a node is never pushed onto a list while it is already on one;
- `reduce_pending_deallocation_count` never underflows (the C++ asserts it).

Deadlock checking is on, which is how an interrupt handler that can never finish would be found (for example one spinning on a manager the code it interrupted is trimming).

## Abstractions

- Memory is counted in 64-byte units. Metadata record `i` sits at unit `ArenaUnits-1-i`, so the frontier/metadata collision checks are the C++ ones.
- `intrusive_tagged_stack` push and pop are single atomic steps on a set, and pop order is nondeterministic, which covers LIFO. The tag counter is what makes the C++ CAS ABA-safe; `lockfree_tagged_stack/` models the stack itself.
- The 8 address bins are collapsed into one list per (size class, shard). Each size in `Sizes` is its own size class.
- Packed-state version counters are not modelled. Every allocation is freed exactly once, by the thread holding it (the API contract), so a version never decides a CAS. A double free of a stale pointer is therefore outside the model.
- A window walks the stolen pending list head-first (LIFO) and hands `Threshold` entries at a time to `process_pending_batch`, as the C++ does with `MAINTENANCE_WINDOW_BATCH_SIZE`. The scan of one batch in `reclaim_pending_frontier_batch` (one state load per entry) is a single step.
- A spin in `reserve_active_slot()` on a TRIMMING manager is an `await`, so a spin that can never end shows up as a deadlock.

## Configurations and expected results

| Config | Parameters | Expected | Result (TLC 1.8) |
|--------|----------|----------|-----------------------------|
| `small.cfg` | 2 CPUs × 2 ops, clean arena, threshold 1 | no error | no error; 2,259,464 distinct states |
| `batch2.cfg` | 2 CPUs × 2 ops, threshold and batch size 2 | no error | no error; 1,341,697 distinct states |
| `irq.cfg` | 1 CPU (3 ops) + interrupt handler (2 ops) | no error, no deadlock | no error; 1,506,856 distinct states |
| `stale_backlink.cfg` | 2 CPUs, 1+3 ops, `StaleBacklinkAt5` | no error | no error; 956,908 distinct states |
| `small_live.cfg` | 2 CPUs (2 + 1 ops), `SpecIFair` (weak fairness per process) | no error; `Termination` (no livelock in the spin/undo loops) | no error, `Termination` holds; 95,417 distinct states |
| `irq_live.cfg` | as `irq.cfg`, `SpecIFair` | no error; `Termination` | no error, `Termination` holds; 1,506,856 distinct states |
| `irq2.cfg` | 2 CPUs × 1 op, each with an interrupt handler (1 op) | no error, no deadlock | no error in 13,254,117 distinct states (stopped after 50 min with TSan sharing the CPUs, not exhaustive; 811,561 states left on the queue); a run from before the switches were removed |
| `irq2_small.cfg` | as `irq2.cfg` with one block size (2) and 10 arena units | no error, no deadlock | no error; 30,846,135 distinct states (exhaustive, 70 min on 4 workers) |

## Running

```bash
./run_tlc.sh small.cfg                       # exhaustive, ~6 minutes on 3 cores
./run_tlc.sh small.cfg -simulate -depth 600  # random deeper runs
```

`run_tlc.sh` translates the PlusCal (`pcal.trans`) before each run, so edit the algorithm, not the generated TLA+ between `BEGIN TRANSLATION` and `END TRANSLATION`.
