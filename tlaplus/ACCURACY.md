# Model accuracy and coverage of the lock-free classes

For every lock-free class this file covers:
- which model or argument covers it;
- what each model abstracts, and why that is sound;
- what is left as a documented assumption.

The models are sequentially consistent. The orderings they rely on are checked on AArch64 by the herd7 tests in
`litmus/aarch64`.

Run every config with `tlaplus/run_all.sh`. Every configuration listed here is exhaustive unless the row says
otherwise.

## Summary

| Class | Coverage | Deadlock / livelock | Races (C++ and AArch64) | Use-after-free / bad access |
|-------|----------|---------------------|-------------------------|-----------------------------|
| `lockfree_single_arena_resource` | `lockfree_allocator/` (+ `boundary/`) | `Termination` under fairness (`fixed_small_live`, `fixed_irq_live`) | every relaxed access is ordered by a later release of `block_state_` or read after a claim; litmus A | `Safety` (no overlap, header reads in live blocks only) |
| `intrusive_tagged_stack` | `lockfree_tagged_stack/` | `Termination` | link load/store are atomic; MP through head release/acquire | ABA: needs the counter not to wrap during one stall (2^16 head changes; 2^39+ for the static arena's arena-relative heads) |
| `lockfree_static_arena_resource` | tagged stack model (`scribble.cfg`) + bump-pointer argument below | lock-free CAS loops | **fixed:** links in a side table, so a stale `load_link` never reads caller memory | type-stable arena memory |
| `bitblock_set` | `lockfree_bitblock_set/` | TLC deadlock check; CAS loops are lock-free (a failed CAS means another context succeeded) | acquire to take bits, release to return them | `MutualExclusion`, `NoOrphanBits` (small configs exhaustive) |
| `bitset` | `lockfree_bitset/` | TLC deadlock check; single atomic RMWs | single-word RMWs | `MutualExclusion` from the RMW results, with interrupts; a load/store mutation fails it |
| `lockfree_bitblock_resource` | argument below + `bitblock_set` model | bounded retries | **fixed:** hint pointers now release/acquire (litmus E) | blocks never freed while live |
| `skiplist` (lists, epochs) | `lockfree_skiplist/` (`LockfreeSkiplist.tla`, `upper_levels/`) | `GlobalProgress` (lock-freedom) with 2 threads; per-operation completion with 1 | litmus B, C, D | reader reachability, see below; **fixed:** `remove()` re-walk retargeting a re-inserted node |
| `skiplist` slot blocks | `lockfree_skiplist/slot_blocks/` | `Termination` | litmus I, J | **fixed:** cross-list block use-after-free |
| `skiplist` handles, iterators | `lockfree_skiplist/handles/` | `Termination` | acquire on table → generation re-check; retired mark + fence (litmus K) | **fixed:** stale handle resolved to a newer node; **fixed:** iterator `++` read a retired node being freed |
| `mp_sc_growable_ring_queue` | `lockfree_mp_sc_queue/` | `Termination` | litmus G, H | no write to a freed buffer |
| `elimination_array` | `lockfree_small/` | `Termination`, also with a same-CPU interrupt handler | release/acquire on `state_` | n/a |
| `sp_mc_value_stack` | `lockfree_small/` | `Termination` | release sequence on `state_` | ABA: 32-bit generation |
| `spsc_queue` | `lockfree_small/` | `Termination` | index release/acquire | no slot shared |
| `token_dispenser` | `lockfree_small/` | `Termination`; blocks only while every token is held (intended) | CAS / `and_fetch` | n/a |

## `lockfree_single_arena_resource` (`lockfree_allocator/LockfreeSingleArena.tla`)

The model was re-derived label by label from the current header. The frontier search and allocation
(`get_next_empty_memory_block`), the bins, pending shards, metadata managers, and the boundary re-check and undo all
match the code.

**Abstractions:**
- **Version counters in `block_state_` and the tagged heads are not modelled.** Dropping a counter makes more CASes
  succeed, never fewer, so the model over-approximates the code's behaviours and is sound for safety. The tagged
  heads' ABA protection is checked separately by `lockfree_tagged_stack/`, which is what allows the model to treat
  stack operations as atomic.
- **Address bins collapsed to a single bin per size and shard.** This is an over-approximation: any block can be
  found by any search of its size.
- **Batch frontier reclaim is a single step.** The batch is private to the reclaimer once taken, and frontier
  rollback is serialised by `frontier_reclaim_in_progress_` (N4), so nothing else can interleave with it.
- **Undo loops** (`FB_undo`, `GM_bundo`, `GM_breserve`) wait only on a later or deeper context. `fixed_small_live`
  and `fixed_irq_live` check that they terminate under weak fairness: no livelock.

**Cross-CPU interrupts:** `fixed_irq2.cfg` is not exhaustive. `fixed_irq2_small.cfg` (one block size, smaller arena)
is exhaustive: no error and no deadlock in 31,195,287 distinct states.

**Weak memory:**
- The relaxed header writes (`metadata_index_`, `size_including_header_`, `previous_block_`) all happen before the
  release store of `block_state_`.
- Readers either validate against an acquired `block_state_` (the early `metadata_index_` read is bounds-checked and
  then validated), or read only after claiming the block with a CAS.
- The SB patterns at the frontier/metadata boundary are litmus A.

## `intrusive_tagged_stack` and `lockfree_static_arena_resource`

**Static arena:** the bump pointer is a monotone CAS on `current_ptr_`, so each success owns a fresh, disjoint range.
Everything else is `intrusive_tagged_stack` operations:
- class bins: `pop`, `push`;
- bin 7: `steal_all`, a private best-fit splice through `store_link`, then `push` back.

`scribble.cfg` lets owners overwrite link words with any value, which covers both the splice and user data written
over a free block. It shows that a stale popper's CAS still fails.

**Fixed race:** the free-bin links used to be the second word of each free block.
- A popper stalled after loading the head could atomically `load_link` a block that another thread had meanwhile
  popped, handed to its caller, and started writing with plain stores: a data race in C++ terms, reported by TSan
  in `AllocatorModelTests.ConcurrentStaticArena`.
- The links now live in a side table after the blocks, one 4-byte `link_cell` per 64-byte block (1/17 of the
  buffer). A stale popper reads only a cell, which no caller ever writes. The block keeps only its size word, which
  is written by the freeing owner and read only by the thread that stole bin 7.

**Bin 7:** it can look empty to a concurrent large allocation while one is being searched. That is a false
out-of-memory for large blocks, not a safety issue. It is documented in the header.

## `bitblock_set` (`lockfree_bitblock_set/`) and `bitset` (`lockfree_bitset/`)

- **`bitset`:** each acquire/release is one RMW whose returned value decides ownership; every context (CPU ×
  interrupt depth) may try any bit, and failed acquires are modelled. `MutualExclusion` holds; splitting the RMW into a
  load and a store (`LockfreeBitset_nonatomic_bug.cfg`) breaks it.
- **`bitblock_set`:** the model covers acquire and release of any range of at most 16 bits across at most two words.
  That includes the unconditional `fetch_or` with rollback of only the newly set bits.
  - The scan paths (`acquire_first_available`, `acquire_first_available_contiguous`) pick a candidate range from
    relaxed loads and then run exactly that acquire, so they are covered by the model's arbitrary choice of range.
  - The search hint is only an index (now: the position after the last acquire, or the last freed position), so a
    stale or racing value costs a longer scan and nothing else.

## `lockfree_bitblock_resource` (argument; no separate model)

- **Memory:** blocks are never freed while the resource is live (only in the destructor). Every `block *` that is
  loaded points to live memory.
- **Chain:** `next_block_` is written before the block is published by the acq_rel CAS on `first_block_`, and never
  changes after that. Readers that load `first_block_` with acquire synchronise with every earlier publication,
  because each later CAS continues the release sequence.
- **Termination:**
  - every pass of `arena::do_allocate` that finds nothing consumes a `try_reserve_block` reservation, bounded by
    `MAX_NUMBER_OF_BLOCKS`;
  - the cross-arena fallback is a bounded loop;
  - bit acquisition is the `bitblock_set` model.
- **`free_count_`** is only a filter. The matching `fetch_sub` always happens before the `fetch_add` of the same
  elements, because the user hands the allocation off, so it never underflows. A transiently low value only sends an
  allocator to another block.
- **Fixed in this change:** `core_hints_[...].last_block` and `arena::allocation_hint_` were stored and loaded
  relaxed.
  - A thread reading a hint may never have acquired `first_block_` since the hinted block was constructed, and it then
    reads the block's plain fields. That is a C++ data race.
  - On AArch64 the address dependency kept it safe in practice: `E-bitblock-hint-*-old` is Never, and `-old-nodep` is
    Sometimes.
  - Hints are now stored release and loaded acquire (`E-bitblock-hint-*-fixed` is Never).

## Skip list

**`LockfreeSkiplist.tla`** models control flow, the key set, epochs and tombstone helping, at the level of keys. It
was re-derived from the current `remove()`: the level-0 mark is the deletion, bounded retries return `false`, and a
CAS fails only after another thread changed a link (so two threads cannot fail each other forever without either
changing anything). With two threads it checks lock-freedom (`GlobalProgress`: some operation keeps completing) and
that a `remove()` call deletes at most one node. It does not model memory reclamation, so it says nothing about
use-after-free. That is covered by:
- **The retire point.** `retire_node` is called only by the thread whose CAS removes the node from level 0, after the
  upper levels have drained (`upper_levels/` and litmus B). So a node is unreachable from the list when it is stamped.
- **The stamp ordering.** The final unlink must be ordered before the `retired_epoch_` read. Otherwise a reader that
  enters in a later epoch can still see the node while its stamp is older.
  - On AArch64 the acq_rel `retirement_enqueued_` CAS in between happens to provide this (`D-retire-epoch-*-code`
    is Never; the `-control` without it is Sometimes on LL/SC + RCpc).
  - C++ promises it only with a seq_cst fence, which `retire_node` now issues. The same holds for stamping retired slot
    blocks in `advance_and_reclaim` (litmus J).
- **The reclaimer's reader check:** litmus C.
- **`remove()` retargeting (fixed):** after a failed level-0 mark, `remove()`'s re-walk could land on a node that
  re-inserted its key after another thread removed the original, and mark only its level 0. That node then either
  leaked (never drained) or, if its inserter was still publishing, was retired and then published at an upper level
  (use-after-free). `remove()` now restarts instead (`upper_levels/`, `RetargetBug`; regression test
  `RemoveThatLandsOnAReinsertedNodeReclaimsIt`).
- **`slot_blocks/`:** slot blocks shared between lists of one type (fixed).
- **`handles/`:** stale-handle resolution (fixed), and iterators resolving a handle kept between read sections
  (fixed: a use-after-free found by the stress test, see that README).

**Lifecycle precondition:** constructing the first list of a type concurrently with destroying the last one is not
supported. The destructor's last-instance path resets the static slot tables. This is in addition to the general rule
that a list must not be used while it is being destroyed.

## `mp_sc_growable_ring_queue` and the small classes

See `lockfree_mp_sc_queue/README.md` and `lockfree_small/README.md`.

## Tag and generation widths (assumptions)

| Where | Width | Assumption |
|-------|-------|------------|
| `intrusive_tagged_stack` counter, single arena | 16 bits | a popper does not stall across 65,536 head changes of one stack |
| `intrusive_tagged_stack` counter, static arena (`arena_tagged_ptr`) | 64 − index bits (39 for a 1 GiB arena) | a popper does not stall across 2^39 head changes of one bin |
| `sp_mc_value_stack` generation | 32 bits | a popper does not stall across 2^32 pushes |
| skip-list slot generation | `GENERATION_MASK` bits, seeded per block incarnation | a handle does not outlive a full wrap of its slot's generation |

The models with small tag widths (`tag_wraps.cfg`, `SpMcValueStack_genwrap.cfg`) show what each counter prevents.
