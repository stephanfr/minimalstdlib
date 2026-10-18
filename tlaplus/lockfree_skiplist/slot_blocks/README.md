# Skip-list slot blocks shared between lists

`SkiplistSlotBlocks.tla` models when a retired slot block (`atomic_forward_link::slot_block` in
`include/lockfree/skiplist`) may be freed.

## The hazard

Slot blocks are static per skip-list type and `Tag`, so every live list of the type shares them. `unpack_ptr()` and
`pin_block()` load `blocks_[idx]` and then read the block: its slot generation, its slot table, or `active_slots_`.

- Readers hold handles after the nodes behind them are reclaimed. A link in a retired-but-unreclaimed node can name a
  slot whose node is gone, so a reader can resolve a handle into any block.
- The only thing keeping the block's memory valid is that it is freed after every reader that loaded it has left its
  read section.

Once `retire_block()` has nulled `blocks_[idx]`, no reader can load the block again. Only readers active at that moment
can hold it, and they can belong to *any* live list of the type. Each list's epoch reclaimer tracks only its own
readers.

**At d5d12c1:**
- any list's `advance_and_reclaim()` took the shared retired-block list and freed it after its own readers;
- any list's destructor freed the shared list at once.

A reader of another live list could be reading the block.

## The fix (the code now)

- `retire_block()` does not retire an empty block while more than one list of the type is live. The block stays
  installed and its slots are reused, so memory is bounded by the peak block count.
- `advance_and_reclaim()` takes the shared retired-block list only while its list is the only live one. Lists destroyed
  since had no readers left; lists constructed since cannot load the block.
- A destructor frees the shared list only when it is the sole or last live list. Otherwise it leaves the list for the
  remaining lists.
- `sole_live_instance()` and the constructor each issue a seq_cst fence around the live-list count. A list that retired
  a block and sees one live list cannot miss a list being constructed whose readers might load the block (a
  store-buffering pattern; `litmus/aarch64`).

## Model

**Processes:**
- readers per list (enter, load `blocks_`, use the block, exit);
- a slot process that installs blocks and retires them when their last slot is freed;
- one process per list that extracts and stamps retired blocks, frees them after its readers' grace, and destroys
  the list.

Lists can be constructed late (`LateStart`).

**Abstractions:** the epoch grace is reduced to its guarantee (`LockfreeSkiplist.tla` models the epochs themselves),
and there is one block index. The model checks the extraction and destructor rules but not the retire gating. The gating only removes behaviours, so safety shown without it holds with it.

**Checked:**
- `NoAccessToFreedBlock`: every read of block memory is of a live block.
- No double free (asserts).
- `NoLeakAtEnd`: when every list is gone, every block was freed.
- `Termination`, under weak fairness.

| Config | Lists | Expected | Result |
|--------|-------|----------|--------|
| `two_lists.cfg` | 2, fix | no error, terminates | no error, 3,630,678 distinct states |
| `late_list.cfg` | 2 (one constructed late), fix | no error, terminates | no error, 4,206,813 distinct states |
| `three_lists.cfg` | 3 (one late), fix | no error, terminates | no error, 354,655 distinct states |

C++ tests (`test/correctness/lockfree_skiplist_test.cpp`):
- `SlotBlocksAreNotRetiredWhileSeveralListsOfTheTypeAreLive`: blocks retired by list A were freed while list B was
  live before the fix.
- `SharedSlotBlocksSurviveOtherInstancesReclaimAndDestruction`: a stress run for ASan/TSan. It did not reproduce the
  race before the fix, because the window is a few instructions; it is regression coverage.
