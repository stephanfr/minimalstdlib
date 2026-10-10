# TLA+ model of `mp_sc_growable_ring_queue`

`MpScGrowableRingQueue.tla` models `include/lockfree/mp_sc_growable_ring_queue`.

## How the queue works

- **Producers** claim slots with `fetch_add` on a `(capacity, position)` word, `enqueue_pos_`. Each one then reads
  `buffer_`, constructs the element and stores `ready_` (release).
- **The consumer** pops in order. When a life of a segment is fully drained, `retire_and_grow` frees the old buffer,
  installs a larger one and its capacity, and opens the next life with `enqueue_pos_.store(pack(new_cap, 0))`.
- **`maybe_shrink_active_segment`** closes a lightly loaded life early: CAS `(cap, pos) -> (pos, pos)`.

## Model

The model has one label per shared access. Buffers are numbered and freed explicitly. A producer that reads `buffer_`
records which buffer it will write, and an assertion fails if that buffer has been freed by the time it writes.

**Abstractions:**
- Elements are producer-tagged integers.
- The position clamp needs 2^30 failed claims, so it is out of reach at these sizes.
- Allocation never fails. The recycle-in-place path (used when `MaxBuffers` is reached) reuses a buffer that is never
  freed, which is strictly safer.

**Checked:**
- no write into a freed or replaced buffer (asserts);
- `NoGhostOrDuplicate`;
- `NothingLostAtEnd`;
- `Termination` of producers and consumer under weak fairness.

| Config | What | Expected | Result |
|--------|------|----------|--------|
| `grow.cfg` | 2 producers × 2 pushes, capacity 1 growing to 2 | no error, terminates | no error, 84,941 distinct states |
| `shrink.cfg` | as grow, with early close (`UseShrink`) | no error, terminates | no error, 24,107 distinct states |

**Mutation check:** opening the new life (the `enqueue_pos_` store) before installing the new buffer makes the
buffer assertion fail. The model therefore detects the hazard that the ordering in `retire_and_grow` prevents.

## Weak memory

The model is sequentially consistent. Two publications carry it on AArch64, and both are checked in `litmus/aarch64`:

- **Slot publication:** element construction, then `ready_` (release), against the consumer's acquire load.
  `H-mpsc-ready`.
- **Regrow:** `buffer_` and `capacity_` writes, then `enqueue_pos_` (release store), against the producer's
  acquire `fetch_add`. `G-mpsc-regrow`.

The consumer's relaxed `ready_ = false` store is ordered before the next life's producers by the same release store.
