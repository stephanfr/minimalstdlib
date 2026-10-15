# Lockfree Skiplist TLA+ Model

`LockfreeSkiplist.tla` models the control flow of `include/lockfree/skiplist` that matters for deadlock, progress and
the epoch read sections, at the level of keys (a key stands for the node that currently holds it):

- insert, remove, find and the public `advance_and_reclaim()` are state machines with the code's retry structure;
- a key is `present`, `tomb` (deleted at level 0 but still linked), or `retired` (unlinked; `fullyUnlinked` gates
  reclamation);
- CAS failures are causal: every step that changes a link gives the other threads a token, and a failure consumes the
  failing thread's token. A thread can therefore only lose a CAS after another thread changed something, as in the
  code;
- each key's node has a height chosen at insert, and the upper levels a `remove()` has already tombstoned are tracked,
  so re-marking a tombstoned level is not a change;
- retries are bounded by `MaxRetryTicks` for finite-state checking;
- interrupt re-entrancy is modelled with a per-CPU `readerDepth` nesting counter.

## Fidelity notes

Re-derived from the current code:
- **insert:** `RETRY_INSERT`: search; `FOUND` returns false; a level-0 CAS, whose failures are retried a bounded number
  of times before returning false; then the upper levels are published (a failure skips a level) and it returns true.
- **remove:** `RETRY_REMOVE`: search; not `FOUND` returns false.
  - **Upper marks:** it tombstones the upper levels top-down. A failed `soft_delete` is retried, and after
    `MAX_REMOVE_MARK_RETRIES_PER_LEVEL` the call restarts. These marks delete nothing.
  - **Level-0 mark:** the mark CAS is the deletion (`present` → `tomb`), and the call returns true right after it. The
    node is unlinked and retired then only if its upper levels have drained; otherwise a later traversal does it.
  - **Mark already present:** another `remove()` won, and the call returns false.
  - **Lost CAS:** a CAS lost to a successor change is retried. The re-walk may restart the call: on a stale handle, or
    when the key now belongs to a re-inserted node, which is the retarget fix (see `upper_levels/`).
    `MAX_REMOVE_BOTTOM_RETRIES` returns false.
- **find:** a read-section traversal that may unlink and retire a drained tombstoned node (the helper path every
  traversal takes).
- **advance_and_reclaim:** `CanAdvanceEpoch` and `CanReclaim` are the per-slot reader checks of `try_advance_epoch` and
  `reclaim_from_per_cpu_list`, which self-exempt the calling slot. Reclamation needs `FULLY_UNLINKED` and the epoch
  lag.
- **Interrupt re-entrancy:** only the outermost `enter_read_section` on a CPU records the epoch.

Abstracted:
- **Pointers:** pointer identity, the per-level predecessor graph and memory reclamation itself are not modelled.
- **Sentinels:** the head and tail nodes are implicit. In the code they are recognised by identity, not by key value,
  so every key value is available to callers and the modelled key set needs no reserved values.
- **Tombstoned keys on insert:** an insert that meets a not-yet-unlinked tombstone of its key is modelled as rejecting.
  The code links a new node beside it, but a key cannot be present and tombstoned at once at this level of
  abstraction.
- **Contention:** a token is granted by any link change, related or not. That over-approximates when a CAS can fail.

Not modelled here (each covered elsewhere):
- **Upper-level publication vs. removal** (`upper_levels_remaining_`, `upper_levels_drained()`). This model treats a node's upper levels as cleared before its level-0 unlink. In the code an inserter can still be publishing upper levels while a remover tombstones the node, so the remover leaves level 0 linked until the count drains. That protocol is modelled in `upper_levels/` (below).
- **The slot allocator** behind the forward links (tagged free list, cursor-block pinning, generation seeding). It is covered by the unit tests in `test/correctness/lockfree_skiplist_test.cpp`.
- **Memory ordering.** TLA+ steps are sequentially consistent; the seq_cst fences in `upper_levels_drained()` and the epoch reclaimer are what make the modelled behaviour hold on AArch64.

## upper_levels/

`SkiplistUpperLevels.tla` follows one node from the moment its level-0 link is published:
- an inserter publishing upper levels: reserve (`upper_levels_remaining_ += 1`), re-aim its own link (abandon if tombstoned), CAS the predecessor (retry or skip on failure);
- a remover: tombstone its own links top-down, then level 0; its traversal unlinks the upper links it sees; it unlinks level 0 and retires only if `upper_levels_drained()`;
- helping traversals: unlink tombstoned upper links (`-1` each), and retire the node once it is drained.

`DrainGate = FALSE` models the code before 68ab76f (no count; the remover always unlinks level 0 and retires).

`RetargetBug = TRUE` models `remove()` before the retarget fix. After a failed level-0 mark, its re-walk could land on
this node (a re-insert of a key whose original node another thread had removed) and mark only level 0, skipping the
upper levels. The fix restarts the remove, which is the normal remover (`RetargetBug = FALSE`).

| Config | Expected | Result |
|--------|----------|--------|
| `SkiplistUpperLevels_bug.cfg` (height 3, 1 helper, gate off) | `RetiredUnreachable` violated | violated: the inserter passes its own-link check, the remover marks and unlinks what it sees, the inserter's CAS publishes level 1, the remover retires a node still linked at level 1 |
| `SkiplistUpperLevels.cfg` (height 3, 2 helpers) | no error; `Termination`, `EventuallyRetired` | no error, 10,160 distinct states |
| `SkiplistUpperLevels_tall.cfg` (height 4, 3 helpers, 2 CAS failures per level) | no error; `Termination`, `EventuallyRetired` | no error, 579,519 distinct states |
| `SkiplistUpperLevels_retarget_bug.cfg` (height 3, 2 helpers, `RetargetBug`) | `Safety` violated | violated: the remover marks level 0 before the inserter reserves level 1, sees the count drained, unlinks level 0 and retires; the inserter's own link is unmarked, so it publishes level 1 and a retired node is reachable (use-after-free). If the inserter had already finished, the node instead never drains and leaks (`RemoveThatLandsOnAReinsertedNodeReclaimsIt`) |

```bash
cd upper_levels
java -cp ../../tla2tools.jar pcal.trans -nocfg SkiplistUpperLevels.tla
java -cp ../../tla2tools.jar tlc2.TLC -config SkiplistUpperLevels.cfg -workers auto SkiplistUpperLevels.tla
```

## Files

- `LockfreeSkiplist.tla`: the specification.
- `LockfreeSkiplist_deadlock.cfg`: deadlock and safety invariants (2 keys, 1 thread, 1 CPU).
- `LockfreeSkiplist_interrupt_reentrancy.cfg`: **kernel/ISR re-entrancy**, 2 threads sharing 1 CPU, `MaxReadDepth=3`.
- `LockfreeSkiplist_safety_2t.cfg`: safety with real contention (2 keys, 2 threads, 2 CPUs).
- `LockfreeSkiplist_progress_2t.cfg`: **lock-freedom** (`GlobalProgress`) with 2 threads on 2 CPUs.
- `LockfreeSkiplist_liveness.cfg`: `ActiveOpsEventuallyFinish` (every operation finishes) with 1 thread.
- `LockfreeSkiplist_remove_liveness.cfg`: remove-only `RemoveBottomEventuallyIdle`.
- `LockfreeSkiplist_remove_mark_livelock.cfg`: `RemoveNoMarkLivelock`.

Every configuration also checks `RemoveDeletesAtMostOne`: one `remove()` call deletes at most one node.

`ActiveOpsEventuallyFinish` is checked with one thread only. With two, a `remove()` can restart for as long as the
other thread keeps completing operations. Lock-freedom allows that, and `GlobalProgress` is the property that
applies.

## Run TLC

```bash
cd tlaplus/lockfree_skiplist
java -XX:+UseParallelGC -Xmx4g -cp ../tla2tools.jar tlc2.TLC -workers auto -config LockfreeSkiplist_progress_2t.cfg LockfreeSkiplist.tla
# or every configuration under tlaplus/:  ../run_all.sh skiplist
```

## Results (TLC 1.8, exhaustive)

| Config | States | Result |
|---|---|---|
| `deadlock` (2 keys, 1T, 1C) | 19,813 distinct | no deadlock; all invariants hold |
| `interrupt_reentrancy` (2 keys, 2T, 1C, depth 3) | 13,828,925 distinct | `InterruptReentrancySafe` and all invariants hold |
| `safety_2t` (2 keys, 2T, 2C; ~25 min, in `run_all.sh`'s slow set) | 42,593,176 distinct | all invariants hold, no deadlock |
| `progress_2t` (1 key, 2T, 2C) | 600,541 distinct | `GlobalProgress` holds under weak fairness; all invariants hold |
| `liveness` (1 key, 1T, 1C) | 837 distinct | `ActiveOpsEventuallyFinish` holds |
| `remove_liveness` (1 key, 1T, 1C) | 23 distinct | `RemoveBottomEventuallyIdle` holds |
| `remove_mark_livelock` (1 key, 1T, 1C) | 23 distinct | `RemoveNoMarkLivelock` holds |

The model before this re-derivation treated the first upper-level mark as the deletion and modelled exhausted
bottom retries as a restart. Under contention, that let one `remove()` call delete two nodes, which the code cannot
do. Its liveness configurations used a single thread, so they never exercised contention.

### Kernel/ISR re-entrancy

`interrupt_reentrancy` runs 2 threads on one CPU with `MaxReadDepth = 3`. It checks exhaustively that:

1. a thread inside a read section, for example a kernel thread in `insert`, can be interrupted by a handler that also
   calls a skiplist operation;
2. the nested handler inherits the outermost epoch snapshot without overwriting it;
3. `InterruptReentrancySafe` holds: every CPU with `readerDepth ≥ 1` has a non-zero epoch snapshot;
4. `StateConsistency` (`present ∩ tomb = {}`), `FullyUnlinkedSubsetRetired` and `RemoveDeletesAtMostOne` hold
   throughout.
