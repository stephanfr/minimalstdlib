# `mp_sc_growable_ring_queue` — Design, Trade-offs, and Usage Guide

## Overview

`mp_sc_growable_ring_queue<T, SegmentCount>` is a lock-free, **multi-producer / single-consumer (MPSC)** queue that dynamically adjusts its capacity through a fixed ring of reusable segments. It is designed for workloads where many threads emit items and one dedicated thread consumes them — logging, telemetry, work dispatch, packet ingress.

```cpp
// Minimal usage
minstd::allocator<minstd::mp_sc_growable_ring_queue<MyEvent>::slot_type> alloc;
minstd::mp_sc_growable_ring_queue<MyEvent> q(alloc, /*initial_capacity=*/256);

// Any thread:
q.push_back(MyEvent{...});

// Consumer thread only:
MyEvent e;
if (q.pop_front(e)) { /* process e */ }
```

---

## Design

### Ring of Pre-Allocated Segments

The queue holds `SegmentCount` (default 2) segment objects arranged in a compile-time ring. All segments are allocated at construction time and reused for the lifetime of the queue — no segment is ever dynamically created or destroyed on the hot path.

```
 producer_target_ ─►  Segment 0  ──►  Segment 1
                           ▲               │
                           └───────────────┘
 consumer_target_ ─►  Segment 0
```

Each segment has a **current life**:

1. **Open** — buffer allocated, all slots initialised, counters reset.
2. **Active** — producers claim slots via an atomic fetch-add; the consumer drains sequentially.
3. **Retired** — when the consumer has read every slot in the segment, it frees the old buffer and opens a new one at a (possibly larger or smaller) capacity, then the segment re-enters rotation.

Because retirement waits for full drain, **growth never copies live data** — it only reallocates empty memory.

### Tagged Enqueue State

The producer-side atomic word (`enqueue_pos_`) packs a **32-bit capacity** and a **32-bit ascending position** into a single 64-bit value:

```
 63                32 31                 0
 ┌──────────────────┬───────────────────┐
 │   capacity (32b) │   position (32b)  │
 └──────────────────┴───────────────────┘
```

This solves a subtle safety hazard: a producer that claims a position and is then suspended for an arbitrary duration could wake up after the segment has drained and regrown to a larger capacity. Without the packing, the stale overflow position could look valid against the new, larger capacity, corrupting data. Because the capacity used for the bound check comes from the **same atomic word as the position**, it is frozen at the instant of the `fetch_add` and can never reflect a later life — the check is always self-consistent.

The 32/32 split provides ~4.29 billion headroom on each side, which the implementation treats as physically implausible to exceed (see [Known Limitations](#known-limitations)).

### Cache-Line Layout

Each segment is split across three dedicated cache lines, enforced by `static_assert` offsets:

| Cache line | Fields | Written by |
|---|---|---|
| 1 — read-mostly | `next_`, `original_capacity_`, `capacity_`, `buffer_` | Consumer only, at open/regrow |
| 2 — producer-hot | `enqueue_pos_` | Every producer on every `push_back` |
| 3 — consumer-hot | `dequeue_state_` | Consumer on every successful `pop_front` |

The `producer_target_` (atomic) and `consumer_target_` (plain) pointers on the outer queue object are similarly on separate cache lines. This layout eliminates false sharing between the producer fast path and the consumer fast path; violations would cause a compile error.

### Growth

When the consumer finishes draining a segment (`dequeue_state_ == capacity_`), `retire_and_grow` fires:

1. Computes `new_capacity` from the **next** segment's current capacity (a stable reference that ensures all ring members converge to within one growth factor of each other over enough cycles):
   $$\text{new} = \max\!\left(\left\lceil \frac{\text{ref} \times \text{growth\_numerator}}{\text{growth\_denominator}} \right\rceil,\ \text{ref} + 1\right)$$
   clamped to `max_capacity`.
2. Allocates the new buffer **before** freeing the old one, so an allocator failure degrades to "recycle at the old physical capacity" rather than leaving the segment buffer-less.
3. Sets `enqueue_pos_` with a `memory_order_release` store to reopen the segment for producers.

The default growth factor is 5/4 (≈ 1.25×).

### Opportunistic Shrink

When `idle_rotation_interval != 0`, every *N* successful pops the consumer calls `maybe_shrink_active_segment()`:

1. Reads `enqueue_pos_` (a single acquire load).
2. Skips if: nothing has been claimed; the segment is already overflowing (organically done); or claimed slots ≥ 50% of capacity.
3. Attempts a single `compare_exchange_strong` to cap the segment's effective capacity at the current claim count. A racing producer increments `enqueue_pos_` between the load and the CAS, causing the CAS to fail — the check is simply skipped until the next interval.
4. On success, `capacity_` is reduced. If the consumer has already drained to the new boundary, `retire_and_grow` fires immediately; otherwise it fires naturally when draining reaches that point.
5. The next regrow applies the **inverse** of the growth formula (`floor(current × denom / num)`, minimum 1), driving capacity down gradually over successive idle cycles.

### Ordering

| Scenario | Guarantee |
|---|---|
| Two pushes from the **same producer into the same segment** | FIFO: observed in push order |
| Two pushes from the **same producer across a segment boundary** | None |
| Pushes from **different producer threads** | None: arbitrary interleaving |

The relaxation across segment boundaries is intentional. Forcing the consumer to fully drain one segment before touching any other would permanently stall it whenever production stops mid-segment — the common case at any workload boundary.

### Allocation Failure Handling

`retire_and_grow` allocates the new buffer before freeing the old one. If the new allocation fails, the segment recycles its existing physical buffer at its previous capacity rather than leaving the queue in a broken state. The queue continues functioning at its old size. By default (`MINIMAL_STD_FAILURE_POLICY_DEFAULT`), allocation failure aborts; this fallback is only reachable with a non-default policy.

---

## Advantages

**Zero consumer-side atomic contention.**
`pop_front` performs no atomic read-modify-write. The only atomic operation on the consume path is one acquire load of a `bool` (`ready_`) per slot. This is the fundamental advantage over any mutex-based or CAS-heavy dequeue design, and it scales to arbitrarily many producers without increasing consumer cost.

**Scalable producer side.**
Each `push_back` performs exactly one `fetch_add`. Producers do not coordinate with each other beyond that single RMW: no CAS retry loop, no per-producer state, no lock. Adding more producers adds fetch-add contention on `enqueue_pos_`, but no new coordination mechanism.

**No on-demand allocation races.**
Every segment in the ring is pre-allocated. When a producer exhausts one segment and walks to the next, the next segment's buffer already exists. Growth happens on the consumer thread only, serialised by the single-consumer constraint, so no thread races to allocate.

**Adaptive memory footprint.**
`idle_rotation_interval` allows capacity earned during a burst to shrink back after the burst subsides, without waiting for the segment to be completely refilled to its now-inflated capacity — which might never happen once the burst ends.

**`max_capacity` bounds memory under sustained load.**
Without a ceiling, a queue under continuous saturation grows without limit. With `max_capacity` set, the queue converges to a fixed ring size and applies natural backpressure via `push_back` returning `false`.

**Cache discipline is compiler-enforced.**
The three-group cache-line layout is not a convention enforced only by comments; the `static_assert` offsets make a future field reordering a compile error rather than a silent performance regression.

**Allocation-failure-resilient on the grow path.**
Rather than leaving a segment unusable on a growth allocation failure, the queue silently recycles the segment at its previous capacity and continues operating.

---

## Weaknesses

**Strictly single-consumer.**
The design is single concurrent consumer only and there are no guards to enforce that restriction.

**`pop_front() == false` is ambiguous.**
It means either "no item is available" or "the head slot's producer is transiently mid-publish." A consumer that stops on the first `false` will lose items. A short spin-with-yield before concluding the queue is truly empty is the correct pattern.

**No strict FIFO across the whole queue.**
Items pushed into a newly-opened segment can be consumed before items still pending in an older segment behind a slow producer. Applications requiring total ordering must use sequence numbers at the application layer.

**Growth requires full drain.**
A segment cannot grow while it holds live items. Under sustained saturated load, segments drain and immediately refill, so they keep growing on every cycle until `max_capacity` is reached or memory is exhausted. `idle_rotation_interval` only helps when the queue is not continuously full.

**Destruction is not safe under concurrent producers.**
There is no `close()` or graceful-shutdown primitive. The caller must coordinate externally to ensure all producer threads have exited before the destructor runs.

**Silent truncation at 2³².**
`tagged_enqueue_state` silently truncates capacity or position values at or above 2³². The implementation treats this as physically implausible (a 4.29-billion-slot buffer would require terabytes of memory) and does not assert. See [Known Limitations](#known-limitations).

**Per-segment metadata overhead.**
Each segment occupies at minimum 3 × 64 bytes (three cache lines) regardless of capacity. For very small queues with large `SegmentCount`, this metadata can dominate the total footprint.

**No blocking `push_back`.**
There is no blocking variant. Callers that cannot drop items must implement their own back-pressure mechanism (semaphore, sleep-retry, upstream flow control).

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

| Parameter | Default | Notes |
|---|---|---|
| `allocator` | — | Must outlive the queue. Read by producers and consumer on every construct/destroy. |
| `initial_capacity` | — | Slot count for every segment at construction. All `SegmentCount` segments start at this size. |
| `growth_numerator` / `growth_denominator` | 5 / 4 | Growth multiplier ≈ 1.25×. Applied to the **next** segment's capacity, so ring members converge. |
| `idle_rotation_interval` | 0 (disabled) | Every *N* successful pops, check for under-50%-loaded segment and shrink if found. |
| `max_capacity` | `SIZE_MAX` (unbounded) | Hard ceiling on any segment's grown capacity. Strongly recommended for sustained-load queues. |

---

## Best Practices

### Sizing and `SegmentCount`

- Set `initial_capacity` to the **expected average working set**, not the peak burst size. Segments grow automatically; over-provisioning at construction wastes memory from the start.
- Always set `max_capacity` for queues that will run under sustained load. Without it, capacity grows with every drain cycle and the queue's memory footprint never converges.
- Raise `SegmentCount` (e.g., to 4 or 8) if producers burst faster than the consumer can drain a single segment. Additional segments provide pre-allocated headroom before `push_back` returns `false`, without any new inter-producer coordination cost.

### Handling `push_back() == false`

`false` is a genuine backpressure signal, not an error. Never silently drop it without a deliberate choice:

```cpp
// Option A: spin-yield (bounded wait)
while (!q.push_back(item)) {
    std::this_thread::yield();
}

// Option B: lossy drop (acceptable for telemetry/metrics)
q.push_back(item); // ignore return value intentionally

// Option C: upstream flow control
if (!q.push_back(item)) {
    semaphore.wait(); // block until consumer signals headroom
}
```

### Handling `pop_front() == false`

`false` does **not** mean the queue is empty — a producer may be mid-publish on the head slot. A short spin before concluding empty is required:

```cpp
MyEvent e;
int retries = 0;
while (!q.pop_front(e)) {
    if (++retries > THRESHOLD && q.empty()) break; // truly empty
    cpu_relax(); // pause/yield
}
```

Never treat a single `false` return as end-of-stream.

### Memory Recovery After Bursts

Enable `idle_rotation_interval` if your workload is bursty and you care about memory reclamation after the burst:

```cpp
minstd::mp_sc_growable_ring_queue<MyEvent> q(
    alloc,
    /*initial_capacity=*/  256,
    /*growth_numerator=*/  5,
    /*growth_denominator=*/4,
    /*idle_rotation_interval=*/500,   // check every 500 pops
    /*max_capacity=*/      4096       // stop growing here
);
```

- A value of 100–1000 pops is a reasonable starting point; lower values check more aggressively at a slightly higher per-pop cost.
- Combine with `max_capacity` for queues that must remain cache-resident: `max_capacity` stops upward growth; `idle_rotation_interval` drives downward recovery after a burst.

### Shutdown and Destruction

Always quiesce producers before destroying the queue:

```cpp
stop_flag.store(true, std::memory_order_relaxed);
for (auto& t : producers) t.join();       // wait for all producers to exit
while (q.pop_front(e)) { process(e); }    // drain remaining items
// safe to destroy q here
```

- The destructor does not call `pop_front`. Outstanding items are destroyed by iterating the raw slot buffers; their destructors are called, but no application-level processing happens.
- There is no `reset()` or `clear()`. If re-use between bursts is needed, destroy and reconstruct the queue.

### Ordering

If strict cross-producer ordering is required, embed a monotonic sequence number in each item and re-sort in the consumer:

```cpp
struct MyEvent {
    uint64_t seq;
    /* payload */
};

// Producer:
q.push_back(MyEvent{seq_counter.fetch_add(1), ...});

// Consumer: use a priority queue or reorder buffer keyed on seq.
```

### Multi-Consumer Scenarios

The queue does not support concurrent consumers. For fan-out to multiple workers, shard at the producer level:

```cpp
// One queue per consumer thread
mp_sc_growable_ring_queue<Work> queues[NUM_WORKERS];

// Producers hash or round-robin onto queues[i]
queues[item.partition() % NUM_WORKERS].push_back(item);
```

---

## Appropriate Use Cases

### Well-suited

| Scenario | Why it fits |
|---|---|
| Structured logging / event pipelines | Many threads emit; one thread writes. Order within a thread is preserved; cross-thread interleaving is acceptable. |
| Network packet ingress | Multiple receive threads enqueue; one processing thread dequeues. `max_capacity` bounds memory under traffic spikes. |
| Work dispatch to a single worker | Producer pool emits work items; one worker drains. Worker latency is bounded by the `ready_` spin, not a lock. |
| Telemetry / metrics collection | Burst-tolerant: segments grow to absorb metric storms, then shrink once the reporter catches up. |
| Embedded / bare-metal with custom allocator | Allocator-agnostic; works with pool allocators carving from static memory. |

### Poorly-suited

| Scenario | Why it does not fit |
|---|---|
| Multiple consumer threads | Not supported; shard instead. |
| Total cross-producer ordering required | No guarantee; use sequence numbers and reorder in the consumer. |
| Very small queues with large `SegmentCount` | Per-segment metadata (3 cache lines) dominates at small capacities. |
| `push_back` must never return `false` | No blocking push; callers must tolerate backpressure. |
| Frequent queue destruction and re-creation | All `SegmentCount` segments are fully torn down; consider reuse or `reset` if available. |

---

## Known Limitations

These are acknowledged in the source as accepted trade-offs rather than bugs:

| Limitation | Impact |
|---|---|
| **Silent truncation** at 2³² for capacity or position in `tagged_enqueue_state` | Practically unreachable (would require terabytes of backing memory or billions of simultaneously blocked producers). No assert fires. |
| **`deallocate(nullptr, 0)`** on first-allocation failure at construction | The default failure policy (`MINIMAL_STD_FAILURE_POLICY_DEFAULT`) aborts before this point; only non-default, failure-tolerant policies reach it. |
| **`capacity_` vs `original_capacity_` divergence** after idle shrink | The destructor correctly iterates `original_capacity_` (the physical allocation), not the potentially-reduced `capacity_`. Live-element destruction is guided by the `ready_` flag, not a contiguous prefix assumption. |
| **`test-and-test-and-set` optimisation deliberately absent** on the producer side | A speculative load before the `fetch_add` was measured to cause a consistent 20–35% regression on the 1P/1C fast path. The uncontended fast path is considered more important than softening saturated-ring spinning. |
