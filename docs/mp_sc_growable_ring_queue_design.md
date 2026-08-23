# `mp_sc_growable_ring_queue` Design

## Overview

`mp_sc_growable_ring_queue<T, SegmentCount>` is a lock-free, multi-producer / single-consumer (MPSC) queue that dynamically adjusts its memory footprint through a rotating ring of fixed-count segments. Producers call `push_back()` concurrently without any mutex. The single consumer calls `pop_front()` with no atomic contention beyond reading a per-slot `ready_` flag—zero contention on the dequeue path is the central design goal.

---

## Architecture

### Ring of Segments

The queue holds `SegmentCount` (default 2) segment objects arranged in a fixed, compile-time ring. Segments are not created on demand; they are all allocated at construction time and cycle in place for the lifetime of the queue. A segment has three distinct phases in each of its "lives":

1. **Open** – buffer allocated and all slots initialised; `enqueue_pos_` reset to `(capacity, 0)`.
2. **Active** – producers claim slots via atomic `fetch_add` on `enqueue_pos_`; consumer drains sequentially via `dequeue_state_`.
3. **Retired** – when `dequeue_state_ == capacity_` the consumer calls `retire_and_grow`: the old buffer is freed, a new one is allocated at an increased capacity (or a decreased one if idle shrink fired), and the segment reopens for its next life.

Because retirement happens only after a segment is **fully drained**, growth never moves or copies live data; it merely reallocates empty memory before re-entering rotation.

```
  ┌──────────────────────────────────────────────────┐
  │                    Ring (SegmentCount = 2)        │
  │                                                   │
  │   producer_target_  ──►  Segment 0  ──►  Segment 1
  │                              │                ▲   │
  │                              └────────────────┘   │
  │   consumer_target_ ──►  Segment 0                 │
  └──────────────────────────────────────────────────┘
```

`producer_target_` (atomic) and `consumer_target_` (plain, consumer-only) each point into this ring independently and advance to the next segment when the current one is exhausted or drained, respectively.

### Segment Memory Layout

Each `segment` is split across three cache lines to eliminate false sharing between the hot paths:

| Cache line | Fields | Written by |
|---|---|---|
| 1 – read-mostly | `next_`, `original_capacity_`, `capacity_`, `buffer_` | Consumer only, at open/regrow |
| 2 – producer-hot | `enqueue_pos_` (atomic `uint64_t`) | Every producer on every push |
| 3 – consumer-hot | `dequeue_state_` | Consumer on every successful pop |

Static assertions in the source enforce these offsets so future refactors cannot silently reintroduce false sharing.

### Tagged Enqueue State

`enqueue_pos_` packs a **32-bit capacity** and a **32-bit ascending position** into one 64-bit atomic word. The packing solves a real race: a producer that obtains a position via `fetch_add` and is then suspended for an arbitrary time could wake up after the segment has drained and regrown to a larger capacity, making its originally-invalid (overflow) claim look valid against the new, larger capacity. Because the capacity a producer validates against comes from the **same atomic word as the position it claimed**, it is frozen at the instant of the `fetch_add` and can never reflect a later life. The claim is valid exactly when `idx < capacity_at_claim`; any producer with a stale overflow claim simply returns `false` and moves to the next segment.

The 32/32 split gives ~4.29 billion headroom on each side. A capacity that large would require terabytes of backing memory, and a pileup of 4.29 billion simultaneous blocked producers is physically implausible, so the truncation that would occur at those extremes is an accepted known limitation documented in the source.

---

## Constructor Parameters

```cpp
explicit mp_sc_growable_ring_queue(
    allocator_type& allocator,
    size_t initial_capacity,
    size_t growth_numerator   = 5,
    size_t growth_denominator = 4,
    size_t idle_rotation_interval = 0,
    size_t max_capacity = static_cast<size_t>(-1)
);
```

| Parameter | Purpose |
|---|---|
| `allocator` | Provides `allocate`/`deallocate`/`construct`/`destroy` for `slot_type`. Must outlive the queue. |
| `initial_capacity` | Starting slot count for every segment. |
| `growth_numerator` / `growth_denominator` | Multiplier for organic growth, applied to the **next** segment's current capacity: `ceil(ref × num / denom)`, minimum `ref + 1`. Default ≈ 1.25×. |
| `idle_rotation_interval` | Every _N_ successful pops, check whether the active segment is under 50% claimed. If so, cap it and allow it to reopen smaller. `0` (default) disables shrinking entirely. |
| `max_capacity` | Hard ceiling on the capacity a segment may grow to. Without it, sustained saturated throughput causes unbounded growth. Default is effectively unlimited. |

---

## Growth and Shrink

### Organic Growth

Triggered when `dequeue_state_ == capacity_` (segment fully drained). The new capacity is computed as:

$$\text{new} = \max\!\left(\left\lceil \frac{\text{ref} \times \text{num}}{\text{denom}} \right\rceil,\ \text{ref} + 1\right), \quad \text{clamped to } \text{max\_capacity}$$

where `ref` is the **next** segment's current capacity (a consistent reference point regardless of `SegmentCount`). Over many cycles all segments converge to within one growth factor of each other.

### Opportunistic Shrink

When `idle_rotation_interval != 0`, every _N_ pops the consumer calls `maybe_shrink_active_segment()`, which:

1. Reads the active segment's `enqueue_pos_` (a single acquire load).
2. Skips if no slots have been claimed, if the segment is already overflowing (organically done), or if claimed ≥ 50% of capacity.
3. Attempts a single `compare_exchange_strong` to cap the segment's capacity at its current claim count. A racing producer that increments `enqueue_pos_` between the load and the CAS causes the CAS to fail; the check is simply skipped until the next interval.
4. On success, sets `capacity_` to the capped value. If the consumer has already drained to the new boundary, `retire_and_grow` is called immediately; otherwise it will fire naturally when draining reaches that point.

The next regrow uses the **inverse** of the growth formula (multiply by `denom/num`, floor at 1), so repeated idle cycles drive capacity down gradually rather than all at once.

---

## Ordering Guarantees

| Scenario | Guarantee |
|---|---|
| Two pushes from the **same producer thread into the same segment** | FIFO: consumer observes them in push order |
| Two pushes from the **same producer thread across a segment boundary** | None: the new segment may be observed before the old segment finishes draining |
| Pushes from **different producer threads** | None: arbitrary interleaving |

The relaxation across segment boundaries is intentional and necessary. Requiring the consumer to drain each segment to its exact declared capacity before touching others would permanently stall the consumer whenever production stops mid-segment—the common case at any workload boundary.

---

## Public Interface

### `push_back(Args&&... args) -> bool`

May be called from any number of producer threads concurrently. Attempts to claim a slot in `producer_target_`; on failure walks to the next segment up to `SegmentCount` times. Returns `false` only when every segment in the ring is full. A `false` return is a transient backpressure signal, not a permanent error—callers must handle it (retry, drop, or apply upstream backpressure).

### `pop_front(T& front) -> bool`

**Must be called from exactly one thread.** Returns `true` and moves the head element into `front` if a ready slot is available. Returns `false` if either the queue is empty or a producer is transiently mid-publish on the head slot. **`false` does not definitively mean empty**; callers must retry rather than treating it as end-of-stream.

### `empty() -> bool`

Consumer-thread-only. Scans all segments for a ready slot. A best-effort hint only—`true` means "nothing visible right now" but a concurrent push may be in-flight.

### `capacity_estimate() -> size_t`

Returns the sum of all segments' current declared capacities. Useful for diagnosing growth and verifying that `max_capacity` is taking effect.

---

## Destruction

The destructor assumes **no producer threads are concurrently pushing**. This is the same convention as `spsc_queue`. Callers are responsible for signalling producers to stop and waiting for them to exit before the destructor runs.

For each slot, the destructor checks the `ready_` flag and destroys only the live `T` objects it finds (production may have stopped mid-segment, leaving a mix of claimed and never-claimed slots).

---

## Strengths

- **Zero consumer-side contention.** `pop_front` does no atomic RMW. The only atomic operation on the consume path is an acquire load of a single `bool` (`ready_`) per slot. This is the primary advantage over any mutex-based or CAS-heavy dequeue design.

- **Scalable producer side.** Each `push_back` performs exactly one `fetch_add` on the segment's `enqueue_pos_`. Producers do not coordinate with each other beyond that single RMW; there is no CAS retry loop, no per-producer state, and no lock.

- **No on-demand allocation races.** Every segment in the ring is pre-allocated. When a producer exhausts one segment and walks to the next, the next segment's buffer already exists—no thread races to allocate it. Growth happens on the consumer thread only, serialised by the single-consumer constraint.

- **Adaptive memory footprint.** `idle_rotation_interval` allows capacity earned during a burst to be reclaimed after the burst without waiting for a full refill, which might never happen.

- **Cache-line discipline enforced by static assertions.** The three-group layout is not a comment-only convention; failing to respect it causes a compile error.

- **`max_capacity` bounds memory under sustained load.** Without it, a queue under continuous saturation grows without limit. With it, the queue behaves as a bounded ring at steady state and applies natural backpressure via `push_back` returning `false`.

---

## Weaknesses

- **Strictly single-consumer.** Concurrent multi-consumer `pop_front` was deliberately removed: it required hazard-pointer reclamation of drained buffers, adding significant complexity and a subtle reclamation hazard. Any application needing multi-consumer behaviour must shard (one queue per consumer) or add an external dispatch layer.

- **`pop_front() == false` is ambiguous.** It means either "empty" or "head slot not yet published." Callers that stop on the first `false` will miss items. A common pattern—spin or yield on `false` until a deadline—must be the application's responsibility.

- **No strict FIFO across the whole queue.** Items pushed into a newly-opened segment can be consumed before items still pending in an older segment behind a slow producer. Applications that require total order must either use a single producer or implement sequence numbers at the application layer.

- **Growth requires a full drain.** A segment cannot grow while it still holds live items. Under sustained saturated load the queue keeps growing until either `max_capacity` is reached or memory is exhausted; `idle_rotation_interval` helps only when the queue is not continuously full.

- **Destruction is not safe under concurrent producers.** Unlike some queues that provide a `close()` mechanism, this one offers no graceful shutdown primitive. The caller must coordinate externally.

- **`tagged_enqueue_state` truncates silently at 2³²** (capacity or position). At 4.29 billion slots or producers this would corrupt data. The implementation treats this as physically implausible and does not assert.

- **Memory overhead per segment is at minimum 3 × 64 bytes** (three cache lines for the segment metadata) plus the slot buffer. For very small queues with many segments this overhead can dominate.

---

## Appropriate Use Cases

### Well-suited

| Scenario | Why it fits |
|---|---|
| Logging / event pipelines | Many threads emit; one thread writes to disk or forwards downstream. Order within a thread is sufficient; cross-thread interleaving is acceptable. |
| Network packet ingress | Multiple NIC receive queues or threads enqueue; a single processing thread dequeues. `max_capacity` bounds memory even under traffic spikes. |
| Work distribution to a single worker | Producer pool generates work items; one worker drains and executes. Worker latency is bounded by `ready_` spin, not a mutex. |
| Telemetry / metrics collection | Burst-tolerant: segments grow to absorb metric storms, then shrink once the reporter catches up (with `idle_rotation_interval`). |
| Embedded / bare-metal with a custom allocator | Allocator-agnostic; works with pool allocators that carve from static memory. |

### Poorly suited

| Scenario | Why it does not fit |
|---|---|
| Multiple consumer threads | Not supported; use a sharded design or a different queue. |
| Total ordering required | Cross-segment and cross-producer ordering is not guaranteed. |
| Very short-lived queues with many segments | Per-segment metadata overhead (3 cache lines) dominates at small capacities. |
| Scenarios where `push_back` must never return `false` | No blocking push variant exists; callers must tolerate backpressure. |
| High-frequency destruction and re-creation | All segments are destroyed and reconstructed; there is no reset-to-empty operation. |

---

## Best Practices

### Sizing

- **Set `initial_capacity` to the expected average working set**, not the burst peak. Segments grow automatically; over-provisioning at construction wastes memory from the start.
- **Set `max_capacity`** for any queue that will run under sustained load. Without it, capacity grows with every full-drain cycle and never converges.
- **Increase `SegmentCount`** (e.g., 4 or 8) if producers burst faster than the consumer can drain a single segment. More segments give more pre-allocated headroom before `push_back` returns `false`, without adding coordination cost.

### Backpressure

- **Never ignore `push_back() == false`.** It is a genuine backpressure signal. Typical responses: spin-retry with a yield, drop the item (for lossy telemetry), or block the calling thread on an external semaphore.
- **Never treat `pop_front() == false` as definitive queue-empty.** A short spin (with a yield or pause instruction) before concluding the queue is empty is the correct pattern.

### Memory recovery

- **Enable `idle_rotation_interval`** if your workload is bursty and capacity recovery after a burst matters. A value of 100–1000 successful pops is a reasonable starting point; lower values check more aggressively and incur a slightly higher per-pop cost.
- **Combine `idle_rotation_interval` with `max_capacity`** for queues that must remain cache-resident. `max_capacity` stops upward growth; `idle_rotation_interval` drives downward recovery after a burst.

### Shutdown

Always quiesce producers before destroying the queue:

```cpp
stop_flag.store(true);          // signal producers to exit
for (auto& t : producers) t.join();
// safe to destroy queue now
```

Never rely on the destructor to drain outstanding items; call `pop_front` until the queue is empty before the final join if live items must be processed.

### Ordering

If strict item ordering across producers is required, add a monotonic sequence number to each element and re-sort in the consumer rather than relying on queue order.

---

## Known Limitations (Accepted Nits)

These are documented in the source as accepted limitations rather than bugs:

- **Silent truncation at 2³² for capacity or position.** Practically unreachable, but there is no assert.
- **`deallocate(nullptr, 0)` on first-allocation failure at construction.** The default failure policy (`MINIMAL_STD_FAILURE_POLICY_DEFAULT`) aborts before this point; only non-default policies can reach it.
- **`capacity_` and `original_capacity_` divergence under idle shrink.** Destroy iterates `original_capacity_` (the physical allocation), not `capacity_` (the effective window). This is correct but worth noting when reading the destructor.
