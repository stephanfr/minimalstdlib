# MPSC Growable Ring Queue Design Documentation

## Overview

The `mp_sc_growable_ring_queue` is a **Multi-Producer Single-Consumer (MPSC)** lock-free queue that dynamically adjusts its capacity through a ring-buffer architecture composed of rotating segments. Designed for high-throughput scenarios with multiple concurrent producers and exactly one consumer thread, it offers excellent scalability while maintaining memory efficiency through intelligent growth strategies.

### Key Characteristics

- **Lock-Free Enqueue**: Multiple producer threads can call `push_back()` concurrently without locks or atomic contention
- **Contended-Free Dequeue**: The single consumer experiences zero contention on `pop_front()` operations
- **Dynamic Memory Management**: Segments grow organically (~1.25× by default) when fully drained, shrinking opportunistically under idle conditions
- **Cache-Optimized Layout**: Carefully padded to prevent false sharing between producer and consumer hot paths
- **Allocator-Agnostic**: Works with any C++ allocator via template support

---

## Architecture

### Core Structure

```
┌─────────────────────────────────────────────────────────────┐
│                     Ring Buffer                              │
│  ┌───────────┬───────────┬───────────┬───────────┬───────┐ │
│  │ Segment 0 │ Segment 1 │ Segment 2 │ ...      │Seg N-1│ │
│  │           │           │           │          │       │ │
│  │ [Life A]  │ [Life B]  │ [Life C]  │ ...     │ Life X │ │
│  │◄──────►   │◄──────►   │◄──────►   │ ◄──────►│ ◄─────┘ │
│  └───────────┴───────────┴───────────┴──────────┴─────────┘
│         ▲                  ▲                 ▲             │
│         │                  │                 │             │
│    Producer Target    Rotating Cycle    Consumer Drains    │
└─────────────────────────────────────────────────────────────┘
```

### Segment Lifecycle

Each segment operates through discrete "lives":

1. **Opening Phase**: Segment is allocated with initial capacity
2. **Active Phase**: Producers fill slots; consumers drain sequentially
3. **Retirement**: When fully drained, segment retires old buffer
4. **Regrowth**: New buffer allocated at increased capacity (or decreased if idle-shrunk)
5. **Repeat**: Segment reenters rotation cycle

---

## Template Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `T` | - | Element type stored in the queue |
| `SegmentCount` | 2 | Number of segments in the rotating ring |

### Constraints

- `SegmentCount >= 2`: At least two segments required for proper ring operation

### Trade-offs

**Increasing SegmentCount:**
- ✅ More pre-allocated headroom absorbs bursts before returning `false`
- ✅ Better amortized latency under bursty workloads
- ❓ Higher peak memory usage
- ⚠️ Does NOT eliminate eventual exhaustion under sustained saturation

---

## Constructor Parameters

```cpp
explicit mp_sc_growable_ring_queue(
    allocator_type& allocator,
    size</think>

initial_capacity,
    size_t growth_numerator = 5,
    size_t growth_denominator = 4,
    size_t idle_rotation_interval = 0,
    size_t max_capacity = SIZE_MAX
)
```

### Parameters Explained

#### `allocator`
Standard C++ allocator for managing segment buffers. Any compliant allocator works.

#### `initial_capacity`
Starting number of elements per segment. All segments begin with equal capacity.

#### `growth_numerator/denominator`
Control organic growth ratio. Effective multiplier: `(numerator + denominator - 1) / denominator`

**Examples:**
- Default (5/4): ~1.25× growth per regrow
- Aggressive (3/2): 1.5× growth
- Conservative (1/1): 2× growth

#### `idle_rotation_interval`
Every N successful pops, check if active segment is <50% utilized. If so, close it early to enable shrink-on-next-open. Value of 0 disables this feature entirely.

**Purpose:** Prevents permanent inflation after burst subsides. Without this, a segment that grew during a burst would only shrink after refilling to its inflated capacity—which may never happen if demand drops.

#### `max_capacity`
Hard ceiling on segment capacity. `SIZE_MAX` (default) means unbounded growth. Useful for bounding memory under sustained saturated load.

---

## Public Interface

### `bool push_back(Args&&... args)`

Enqueues element(s) into the queue.

**Thread Safety:**
- ✅ Safe from multiple producer threads concurrently
- ⚠️ Caller must ensure single consumer thread owns `pop_front()`

**Return Value:**
- `true`: Element enqueued successfully
- `false`: All segments currently full (transient condition—retry later)

**Behavior Under Contention:**
When all segments are temporarily full due to concurrent pushes, the method attempts alternative segments in round-robin fashion. Returns `false` only after exhausting all segments.

### `bool pop_front(T& front)`

Dequeues element from queue front.

**Thread Safety:**
- ⚠️ **Must be called exclusively from the single designated consumer thread**
- ❌ Not safe from multiple consumers

**Return Value:**
- `true`: Element dequeued successfully
- `false`: Either queue empty OR producer mid-enqueue (caller must retry)

**Important Distinction:**
Returning `false` does NOT definitively mean the queue is empty—it indicates either emptiness or transient production activity. Always retry on failure.

### `bool empty() const`

Checks if queue appears empty.

**Semantics:**
Returns `true` only if NO ready elements exist across ALL segments. This is a best-effort hint—not a linearizable query—as producers may have published items not yet visible to this check.

### `size_t capacity_estimate() const`

Returns sum of all segments' current capacities.

**Use Case:** Diagnostic tool to verify growth/shrink behavior. Example:
```cpp
if (queue.capacity_estimate() > EXPECTED_CAPACITY) {
    // Growth detected
}
```

---

## Internal Data Structures

### Tagged Enqueue State

Packaging position and lifetime capacity into a single atomic word prevents race conditions where a delayed producer observes stale capacity bounds.

```cpp
struct tagged_enqueue_state {
    static constexpr int CAPACITY_BITS = 32;
    
    // Pack: (capacity << 32) | position
    static storage_type pack(size_t capacity, size_t pos);
    
    // Extract components
    static size_t unpack_capacity(storage_type value);
    static size_t unpack_pos(storage_type value);
};
```

**Why This Matters:**
Without packing, a producer fetching a position could wake after the segment regrew, observing a larger capacity that makes its originally-invalid claim appear valid—a serious corruption risk. Packing binds validity to the exact moment of fetch-add.

### Segment Structure

Three cache-line-aligned groups optimize concurrency:

```cpp
struct segment {
    // Cache Line 1: Read-mostly (producers/consumers read, rarely write)
    segment* next_;              // Fixed ring pointer
    size_t original_capacity_;   // Physical allocation size
    size_t capacity_;            // Current effective capacity
    slot_type* buffer_;          // Element storage
    
    // Cache Line 2: Producer-hot (only producers write)
    atomic<tagged_enqueue_state::storage_type> enqueue_pos_;
    
    // Cache Line 3: Consumer-hot (only consumer writes)
    size_t dequeue_state_;       // Drain cursor for current life
};
```

**Padding Strategy:**
Separating access patterns onto distinct cache lines eliminates false sharing—the primary performance killer in lock-free designs.

---

## Growth Strategies

### Organic Growth

Triggered when a segment reaches full utilization (`dequeue_state_ == capacity_`).

Formula:
```
new_capacity = ceil((old_capacity × numerator + denominator - 1) / denominator)
               but at least old_capacity + 1
               capped at max_capacity
```

**Characteristics:**
- Monotonically increasing
- Converges across all ring segments over time
- Exponential-like expansion (each regrow builds on neighbor's growth)

### Opportunistic Shrink

Enabled via `idle_rotation_interval`. Activates when:
1. Active segment < 50% utilized
2. Fewer claims than half capacity
3. Checked every Nth successful pop

Effect:
- Caps current position as new capacity
- Next regrow shrinks using inverse formula: `ceil(current × denom / numer)`
- Floors at minimum size of 1

**Benefit:** Recovers memory after burst subsidence without requiring full refill cycle.

---

## Usage Patterns

### Basic Pattern

```cpp
#include <lockfree/mp_sc_growable_ring_queue>
#include <minstdconfig.h>
#include <vector>

using namespace MINIMAL_STD_NAMESPACE;

int main() {
    std::vector<my_element_type> pool;
    my_element_type elem;
    
    // Initialize shared queue (one per thread pair typically)
    mp_sc_growable_ring_queue<my_element_type> queue(pool.begin(), pool.end());
    
    // Producer thread
    queue.push_back(elem);
    
    // Consumer thread (ONLY ONE ALLOWED!)
    my_element_type retrieved;
    while (queue.pop_front(retrieved)) {
        // Process retrieved
    }
}
```

### With Custom Allocator

```cpp
class MyResource : public pmr::memory_resource {
public:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return ::operator new(bytes, std::align_val_t(alignment));
    }
    
    void do_deallocate(void* ptr, std::size_t, std::size_t) override {
        ::operator delete(ptr);
    }
    
    bool do_is_equal(const pmr::memory_resource&) const noexcept override {
        return true;
    }
};

MyResource resource;
mp_sc_growable_ring_queue<MyType, 4>(resource, 1024);
```

### High-Burst Scenario

Increase segment count and consider max capacity:

```cpp
// Absorb bursts up to 8K elements before blocking
mp_sc_growable_ring_queue<Data, 8>(allocator, 256, 5, 4, 0, 8192);
```

### Memory-Constrained Sustained Load

Enable shrink and cap growth:

```cpp
// Bound memory, allow recovery after bursts
mp_sc_growable_ring_queue<Data, 2>(
    allocator, 
    512, 
    5, 4,           // moderate growth
    100,            // check every 100 ops
    10000           // hard cap
);
```

---

## Performance Characteristics

### Throughput

- **Uncontested 1P→1C**: Near-linear scaling with core frequency
- **Many Producers → 1 Consumer**: Excellent scale-up; producers contend minimally
- **Bounded vs Unbounded**: Similar throughput; bounded adds minor overhead on hitting cap

### Latency

- **Best-case (empty segment)**: Single CPU cycle for publish/unpublish
- **Worst-case (full ring)**: Spin-wait across segments; proportional to SegmentCount
- **Meaningful metric**: Average latency under realistic workload patterns

### Memory Efficiency

- **Base footprint**: `SegmentCount × initial_capacity × sizeof(element)`
- **Peak under burst**: Additional growth factors accumulate across segments
- **Steady-state with shrink**: Converges near working-set size

---

## Correctness Guarantees

### What It Provides

✅ **Ordering Within Segment**: Two pushes from same producer to same segment preserve FIFO order observable by consumer

❌ **Cross-Segment Ordering**: No guarantee—segment boundaries introduce reordering

❌ **Inter-Producer Ordering**: Different producers' items may interleave arbitrarily

✅ **No Lost Elements**: Every pushed item eventually popped (unless process terminates abnormally)

✅ **No Double-Publish**: Atomic claim mechanism prevents duplicate placement

### What It Doesn't Provide

⚠️ **Multi-Consumer Support**: Deliberately excluded—would require hazard-pointer reclamation of retired buffers

⚠️ **FIFO Across Entire Queue**: Only guaranteed within individual segment lifetimes

⚠️ **Strong Empty Queries**: `empty()` is advisory, not definitive

---

## Failure Modes

### Allocation Failures

Handled gracefully via degradation:

1. **First allocation fails**: Queue construction aborts (OUT_OF_MEMORY exception)
2. **Subsequent allocations fail**: Segment recycles at previous physical capacity
3. **All segments fail**: Eventually triggers OUT_OF_MEMORY

### Shutdown Race Condition

Destructor assumes no concurrent producers. Convention matches SPSC queue:
- Signal shutdown via external flag
- Wait for producers to finish
- Then destroy queue

---

## Testing Coverage

The implementation includes extensive correctness and performance testing:

### Correctness Tests
- Basic single-threaded operations
- Burst absorption dynamics
- Deep-ring buffering behavior
- Idle rotation shrink mechanics
- Contention handling
- Allocation failure resilience
- Capacity capping enforcement

### Performance Benchmarks
- Fixed large capacity (no regrowth scenario)
- Continuous growth patterns
- Multi-strategy comparison (Moody's camel, Jiffy, etc.)
- External queue benchmarking

---

## Related Components

This queue integrates with the broader minimalstdlib ecosystem:

- `<allocator>`: Standard allocator abstraction
- `<__memory_resource/memory_resource.h>`: PMR compatibility layer
- Lock-free primitives: `spsc_queue`, `bitblock_set`, `skiplist`
- Test infrastructure: CppUTest framework

---

## Migration Guide from Other Queues

### From std::deque

| Aspect | std::deque | mp_sc_growable_ring_queue |
|--------|------------|--------------------------|
| Thread safety | Requires mutex | Lock-free (MPSC only) |
| Memory layout | Fragmented blocks | Contiguous rings |
| Growth strategy | Doubling blocks | Per-segment gradual |
| Peak memory | O(N) | O(Segments × Cap) |
| Best for | General-purpose | High-contention producer loads |

### From std::queue (with mutex)

| Benefit | Improvement |
|---------|-------------|
| Removes locking overhead | 10-100× higher throughput typical |
| Eliminates deadlock risks | None possible with single consumer |
| Predictable latency | Worst-case bounded spin |
| Adds complexity | Must manage single-consumer discipline |

---

## Troubleshooting

### "Queue keeps growing forever"

**Symptoms:** `capacity_estimate()` climbing indefinitely under variable load

**Solution:** Enable idle rotation and/or set max_capacity:
```cpp
mp_sc_growable_ring_queue<T, 2>(alloc, 256, 5, 4, 500, 65536);
```

### "Too many false returns from push_back()"

**Cause:** Insufficient segment count for your burst profile

**Diagnosis:** Measure peak simultaneous items needed

**Fix:** Increase SegmentCount:
```cpp
// Try 4, 8, or more depending on burst magnitude
mp_sc_growable_ring_queue<T, 8>(alloc, 128);
```

### "Consumer sees gaps in sequence"

**Reality Check:** Cross-segment ordering isn't guaranteed. If strict sequencing matters:
- Limit to single producer thread, OR
- Implement application-level sequencing outside the queue

### "Out-of-memory despite reasonable limits"

**Checkpoints:**
1. Verify max_capacity is actually being hit
2. Confirm idle rotation is reducing segments post-burst
3. Review SegmentCount × max_capacity product
4. Consider custom allocator with pooling

---

## Future Enhancements (Not Yet Implemented)

Potential improvements tracked for future consideration:

- [ ] True multi-consumer variant (requires hazard pointers)
- [ ] Stealing optimization (consumer pulls from any non-empty segment)
- [ ] Adaptive growth rates (learn optimal numerator/denominator)
- [ ] Batch operations (push/pop multiple elements atomically)
- [ ] Zero-copy transfer integration

---

## References

- Original design inspired by: [moodycamel:: ConcurrentQueue](https://github.com/cameron314/concurrentqueue)
- Adaptations for: Minimal memory footprint, allocator flexibility, embedded-friendly constraints
- License: BSD-style (see LICENSE file)

---

## Quick Reference Card

| Operation | Signature | Thread Role | Notes |
|-----------|-----------|-------------|-------|
| Construct | `mp_sc_growable_ring_queue(alloc, init_cap, ...)` | Any | One-time per queue |
| Destroy | `~mp_sc_growable_ring_queue()` | Last producer | Ensure no concurrent pushes |
| Push | `push_back(args...)` | Producer only | Retry on false |
| Pop | `pop_front(front)` | Consumer ONLY | Retry on false |
| Empty? | `empty()` | Either | Advisory hint |
| Size est. | `capacity_estimate()` | Either | Sum of segment caps |

**Golden Rules:**
1. Exactly one consumer thread ever calls `pop_front()`
2. Handle `false` returns as transient retries
3. Respect destructor precondition (no concurrent producers)
4. Choose SegmentCount based on burst tolerance, not raw speed
