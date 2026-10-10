# TLA+ model of `intrusive_tagged_stack`

`IntrusiveTaggedStack.tla` models the Treiber stack in `include/lockfree/intrusive_tagged_stack`. Its head is a
`(pointer, 16-bit counter)` word. The stack is used by `lockfree_static_arena_resource` (free bins) and
`lockfree_single_arena_resource` (pending-deallocation shards, metadata free lists).

## Model

One label per shared-memory access:

| C++ | Model |
|-----|-------|
| `push`: `head.load`; loop { `store_link(node, head.ptr)`; CAS `head -> (node, counter + 1)` } | `U_load`, `U_link`, `U_cas` |
| `pop`: `head.load`; loop { null → return; `next = load_link(cur)`; CAS `head -> (next, counter + 1)` }; `store_link(cur, null)` | `P_load`, `P_check`, `P_link`, `P_cas`, `P_clear` |
| `steal_all`: CAS loop `head -> (null, counter + 1)` | one step; `StealResetsTag = TRUE` is the old `exchange(make(nullptr))`, which reset the counter to 0 |
| caller writes into a block it owns (the static arena's bin-7 splice via `store_link`; user data over a `free_block_node`) | `Scribble`: an owner sets a node's link word to any value |

- A thread owns the nodes it popped or stole, and may push them back.
- Memory is type-stable: a stale `load_link` of a node owned elsewhere returns whatever the link holds. With
  `Scribble` that can be any value, so the stale popper's CAS is the only protection.
- The counter wraps at `TagMod`. A `TagMod` larger than the number of head changes in a run stands for the documented
  assumption: the 16-bit counter does not wrap during one popper's stall.

**Checked:**
- `NoCycle`;
- `OwnedNotOnStack`, `OwnedOnce`: a node is never both stacked and owned, or owned twice;
- `NothingLost`;
- the popped node is not already owned (assert);
- `Termination` under weak fairness.

## Results

| Config | What | Expected | Result |
|--------|------|----------|--------|
| `pop_push_only.cfg` | pop + push | no error | no error, 18,515 distinct states |
| `steal_advances_tag.cfg` | pop + push + `steal_all` (the code now) | no error | no error, 228,193 distinct states |
| `scribble.cfg` | as above, owners overwrite link words | no error | no error, 663,383 distinct states |
| `steal_resets_tag.cfg` | `steal_all` resets the counter to 0 (d5d12c1) | ABA: `Safety` violated | violated |
| `tag_wraps.cfg` | 2-value counter | ABA: `Safety` violated | violated (what the 16-bit counter guards against) |

**`steal_all` fix:** the old `exchange(make(nullptr))` reset the counter to 0. A popper stalled with `(A, 0)` could
then succeed against a later `(A, 0)` and install a stale next pointer. `steal_all` now advances the counter with a
CAS loop. `lockfree_single_arena_resource::steal_pending_deallocation_list` uses it instead of its own `exchange`.

## Type stability per user

A stale popper reads the link word of a node that may since have been popped and handed out. That needs the node's
memory to stay mapped, and both users meet that:
- **`lockfree_static_arena_resource`:** blocks live in the caller's static arena and are never returned anywhere.
  The user's plain writes into an allocated block can race with a stale popper's atomic `load_link` of its link word.
  The value read is then discarded, because the popper's CAS fails. This is benign in practice but formally a data
  race, documented in `tlaplus/ACCURACY.md`.
- **`lockfree_single_arena_resource`:** pending lists hold arena blocks and metadata records, and neither is ever
  unmapped while the resource is live.
