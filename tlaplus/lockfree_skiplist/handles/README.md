# Slot-handle resolution

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

Under the skip list's epoch protection, a reader should never hold a handle whose node is reclaimed during its read
section. The window may therefore be unreachable from the list itself. The re-check makes `unpack_ptr()` meet its own
contract regardless, at the cost of one load from a line it has just read.

| Config | What | Expected | Result |
|--------|------|----------|--------|
| `current.cfg` | generation checked before the table load only | assertion fails | fails: the reader checks generation 1, the slot is freed and republished as generation 2, and the reader returns node 2 |
| `recheck.cfg` | generation re-checked after the table load | no error, terminates | no error, 41,747 distinct states |
