# Slot-handle resolution

Two models: `SlotHandleResolution.tla` (resolving a stale handle) and `IteratorSavedHandle.tla` (an iterator
resolving a handle it kept between read sections).

## Stale handles

`SlotHandleResolution.tla` models `atomic_forward_link::unpack_ptr()` in `include/lockfree/skiplist` against
`free_slot()` and `publish_slot()` on the same slot.

A link holds a handle `(slot, generation)`. When a handle is stale, `unpack_ptr()` is meant to return null. A handle is
stale when its slot has since been freed, or freed and republished.

**At d5d12c1**, `unpack_ptr()` checked the generation and then loaded the slot table. If the slot was freed and
republished between those two loads, the stale handle resolved to the new life's node instead of null. The memory was
live; the node was the wrong one.

**The fix:** `unpack_ptr()` re-loads the generation after the (acquire) table load. `publish_slot()` stores the new
generation before the new pointer, so a reader that sees the new pointer also sees the new generation and rejects it.
The ordering argument holds on weak memory: the table load is an acquire of the release store that follows the
generation store.

Inside one read section the epoch keeps a handle's node from being reclaimed. Iterators, however, keep a handle
between read sections (below), so this window is reachable from `operator++`: the iterator's slot can be freed and
republished between its two loads.

| Config | What | Expected | Result |
|--------|------|----------|--------|
| `SlotHandleResolution_recheck.cfg` | generation re-checked after the table load | no error, terminates | no error, 41,747 distinct states |

## Iterators (use-after-free, found by the stress test)

`iterator` and `const_iterator` keep their node's handle between `operator++` calls, outside any read section, and
resolve it again in a new section. Epoch reclamation only protects a node from readers whose section began before the
node was retired. The slot table, though, keeps pointing at a retired node until the node is destroyed (`free_slot()`
runs in `~skip_node`).

So `operator++` could resolve its saved handle to a node that was already retired and was being freed. It then read
that node's links. TSan reported exactly this in `SharedSlotBlocksSurviveOtherInstancesReclaimAndDestruction`:
`operator++` reading `next_node` against `destroy_skip_node`. Reverting only the iterator change brings the report
back (run 26 of 30).

**The fix:**
- `retire_node()` records the node's generation in its slot (`mark_retired`), before the seq_cst fence that precedes
  the epoch stamp.
- The iterators resolve with `unpack_unretired_ptr()`. It returns null for a retired node, and the iterator then
  continues from its cached key (`find_lower_bound_after`), as it already did for a reclaimed node.
- Traversals inside a read section still resolve retired nodes as before, since they are protected.

The ordering is a store-buffering pair through the epoch:
- the retirer stores the mark, fences, then reads the epoch for its stamp;
- the iterator stores its snapshot, fences (in `enter_read_section`), then loads the mark.

Either the iterator sees the mark, or the stamp is at least the iterator's snapshot, in which case the node is not
freed until the section ends. `litmus/aarch64` K-iterator-mark: Never with the fence, Sometimes without it, for all
three codegens.

| Config | What | Expected | Result |
|--------|------|----------|--------|
| `IteratorSavedHandle_fixed.cfg` | `unpack_unretired_ptr` | no error, terminates | no error, 4,306 distinct states |
