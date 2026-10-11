# Lock-free classes vs. standard library + mutex

This report compares each lock-free class with its closest standard C/C++ counterpart, protected by an appropriate
mutex, on identical workloads. The suite is `LockfreeVsStdComparisonPerformanceTests`
(`test/performance/lockfree_vs_std_comparison_test.cpp`, with the std side in
`test/performance/external/std_baselines/std_runner.cpp`).

## Method

- **Same work.** Both sides run the same operation sequence, defined in `test/performance/comparison/workload.h`:
  per-thread xorshift streams, block sizes, keys, op mixes and counts. They start behind the same barrier and are
  timed with the same clock.
- **Equal work is checked.** Each side reports a checksum of the work it actually did: allocations made, items moved,
  successful operations. The suite fails if the two differ wherever the outcome is deterministic.
- **Real standard library.** The std side is compiled against libstdc++ (GCC 13.3, `-O3`), not the minstd headers.
- **Repetition.** Each cell is the median of 5 runs, and the suite was run 3 times. The tables show the median of the
  three medians. `±` marks a ratio that was not on the same side of 1.0 in all three runs: treat it as parity. Only 7
  of 140 ratio cells are marked.
- **Host:** x86-64, 4 vCPUs (Intel Xeon @ 2.1 GHz, shared cloud VM), 16 GB. This is not the AArch64 target. With 8
  threads the host is oversubscribed, so lock holders and spinning threads get preempted.
- **Units:** throughput is in millions of operations per second. ratio = lock-free / std, so > 1 means lock-free is
  faster.

## Summary

| Class | std counterpart | 1 thread | 2-4 threads | 8 threads (oversubscribed) |
|---|---|---|---|---|
| `skip_list` | `std::map` + `std::shared_mutex` / `std::mutex` | 0.9x (read-mostly) to 2.4x (write-heavy) | **4.6x to 18x** | 4.5x to 8x |
| `mp_sc_growable_ring_queue` | `std::queue` + `std::mutex` | **11x** | 2.0x to 2.4x | 2.3x |
| `spsc_queue` | bounded `std::queue` + `std::mutex` | **4.5x** (1 producer + 1 consumer) | | |
| `bitset` | `std::bitset` + `std::mutex` | **3.1x** | 3.2x to 4.4x | 3.2x |
| `token_dispenser` | `uint64_t` mask + `std::mutex` | 2.0x | 1.0x to 1.6x | 0.8x |
| `bitblock_set` | `uint64_t` words + `std::mutex` | 1.4x | 0.7x to 1.3x | 0.5x |
| `intrusive_tagged_stack` | `std::vector<T *>` + `std::mutex` | 1.4x | 0.3x to 0.8x | 0.2x |
| `sp_mc_value_stack` | bounded `std::vector` + `std::mutex` | 0.5x | 0.4x to 0.7x | 0.8x |
| `elimination_array` | `std::vector` stack + `std::mutex` | n/a | 0.05x to 0.1x | 0.1x |

**Memory resources.** Each resource is shown twice: against the standard library's pool resources, which are the
mutex-protected counterparts (`std::pmr::synchronized_pool_resource`, and `std::pmr::unsynchronized_pool_resource` +
`std::mutex`), and separately against glibc `malloc`/`free`. Each range covers both workloads (per-thread churn and
cross-thread frees).

| Resource | vs `std::pmr` pools: 1 thread | 2-4 threads | 8 threads | vs `malloc`: 1 thread | 2-4 threads | 8 threads |
|---|---|---|---|---|---|---|
| `lockfree_composite_single_arena_resource` | 1.2x to 1.5x | **3.4x to 26x** | 2.8x to 20x | 0.30x to 0.41x | 0.41x to 0.58x | 0.40x to 0.50x |
| `lockfree_bitblock_resource` | 1.3x to 1.6x | **3.4x to 21x** | 2.8x to 19x | 0.31x to 0.53x | 0.34x to 0.57x | 0.38x to 0.49x |
| `lockfree_single_arena_resource` | 0.7x to 0.8x | 2.3x to 10x | 1.9x to 9.5x | 0.17x to 0.28x | 0.07x to 0.39x | 0.15x to 0.34x |
| `lockfree_static_arena_resource` | 1.0x to 1.3x | 0.9x to 8x | 0.9x to 5x | 0.25x to 0.42x | 0.02x to 0.42x | 0.02x to 0.18x |

Once threads contend, every resource is faster than the pools, except the static arena: in per-thread churn at 4-8
threads it falls to parity (0.9x). Every resource is slower than `malloc` in every cell, for the reason given below.

## Reading the results

**Where lock-free wins clearly:**
- **The skip list.** One lock serialises every `std::map` writer, and `std::shared_mutex` does not help with 5-50%
  writes: its readers still bounce the lock's cache line. The skip list scales to 4 threads, peaking at 18x on the
  write-heavy mix. At 8 threads on 4 cores it drops to 4.5-8x, still well ahead. Single-threaded and read-mostly it
  is at parity with `std::map`.
- **The queues.** The single-producer cases (11x mp_sc, 4.5x spsc) avoid a lock round trip per item entirely. With
  2-8 contending producers the ring stays about 2x ahead.
- **`bitset`.** One atomic RMW replaces a lock/unlock pair: 3-4x at every thread count.
- **The pooled memory resources** (`composite`, `bitblock`) against the standard library's pool resources, which are
  their true counterparts. The pools serialise on a mutex and fall to 0.5-8 Mops/s at 2+ threads. In per-thread
  churn the lock-free resources reach 60-134 Mops/s; with cross-thread frees they reach 6-8 Mops/s, still 3-15x the
  pools. `lockfree_single_arena_resource` (general purpose, any size) wins 2-10x
  once threads contend, and is about 0.8x single-threaded.

**`malloc` is not a mutex-protected baseline.** glibc `malloc` is a thread-caching allocator: per-thread tcache bins and
per-thread arenas. In the per-thread churn workload no two threads touch the same allocator state, so it scales
almost linearly (323 Mops/s at 4 threads) and beats every resource here, by 2-50x. With cross-thread frees it drops to
11-16 Mops/s, and `composite` and `bitblock` come within about 2x. A bare-metal target has no such allocator, which is
what these resources replace. The table includes `malloc` because it is the closest C counterpart, not because it is
mutex-protected.

**Where lock-free loses, and the likely reasons.** These are hypotheses, not measured:
- **One contended word with an immediate retry.** `intrusive_tagged_stack`, `sp_mc_value_stack`, `token_dispenser`,
  `bitblock_set` at 4+ threads, and `lockfree_static_arena_resource` at 4+ threads all funnel every operation through
  a single CAS location, or a few: the stack head, the `(generation, count)` word, the token word, the search hint,
  and the static arena's 3-4 size-class heads.
  - Under contention most CASes fail and retry at once. The benchmark, like the static arena, passes the stack a
    backoff that only counts retries.
  - A `std::mutex` lets one thread run uncontended while the others sleep in the kernel, which on 4 cores often gives
    higher throughput.
  - Exponential backoff, or sharding the hot word per CPU (as the single arena shards its pending lists), would be the
    first things to try.
- **`sp_mc_value_stack`** loses even with one consumer. The producer and the consumer CAS the same word for every
  item, whereas the mutex version lets the producer push several items per lock hand-off.
- **`elimination_array`** is not a container: it is a rendezvous that pairs a `put` with a concurrent `get` in one of
  16 random slots. A `put` that finds no partner within its timing window fails and retries, so on its own it moves
  10-20x fewer items than a mutex stack. It is meant as a backoff layer in front of a stack, not as a stack.

**What the numbers do not show.** The lock-free classes give progress guarantees that a mutex cannot:
- no thread waits on a preempted or interrupted lock holder;
- the allocator and the queues are safe to call from interrupt context on the bare-metal target, where a mutex would
  deadlock against the code it interrupted.

In several "losing" cases the lock-free class is there for that guarantee, not for throughput.

## Detailed results

In the memory-resource tables, the first ratio column is against `malloc`/`free` (a thread-caching allocator, not a
mutex-protected one) and the next two are against the `std::pmr` pool resources (the mutex-protected counterparts).

### bitblock_set<16384>, acquire first free run of 1-4 bits + release

Each thread holds up to 32 runs, releasing its oldest before acquiring the next.  The std side uses the same word-wise search for a run inside one 64-bit word, under a mutex.

| threads | bitblock_set | uint64_t words + std::mutex | ratio |
|---:|---:|---:|---:|
| 1 | 37.30 | 26.89 | 1.39x |
| 2 | 7.70 | 6.10 | 1.26x |
| 4 | 4.70 | 6.53 | 0.72x |
| 8 | 4.14 | 7.75 | 0.53x |

### bitset<4096>, acquire + release of a random bit

Each operation acquires a random bit and, if it was free, releases it (two separate operations, as two lock acquisitions on the std side).

| threads | bitset | std::bitset + std::mutex | ratio |
|---:|---:|---:|---:|
| 1 | 80.21 | 26.08 | 3.08x |
| 2 | 27.02 | 6.16 | 4.39x |
| 4 | 20.32 | 6.33 | 3.21x |
| 8 | 22.33 | 7.01 | 3.19x |

### token_dispenser, get + release

8 tokens; each operation takes a token and returns it.

| threads | token_dispenser | uint64_t mask + std::mutex | ratio |
|---:|---:|---:|---:|
| 1 | 59.57 | 29.32 | 2.03x |
| 2 | 10.07 | 6.26 | 1.61x |
| 4 | 6.83 | 6.98 | 0.98x |
| 8 | 6.58 | 8.09 | 0.81x |

### elimination_array, N/2 producers hand items to N/2 consumers

Throughput counts items handed over.  The elimination array pairs a put with a get directly (a put waits briefly for a partner and retries); the baseline passes items through a mutex-protected std::vector stack.

| threads | elimination_array | std::vector stack + std::mutex | ratio |
|---:|---:|---:|---:|
| 2 | 0.28 | 5.57 | 0.05x |
| 4 | 0.80 | 7.37 | 0.11x |
| 8 | 0.81 | 8.33 | 0.10x |

### intrusive_tagged_stack, pop + push back

64 nodes per thread on one shared stack; each operation pops a node and pushes it back.

| threads | intrusive_tagged_stack | std::vector<node *> + std::mutex | ratio |
|---:|---:|---:|---:|
| 1 | 42.59 | 30.08 | 1.42x |
| 2 | 4.88 | 6.27 | 0.78x ± |
| 4 | 2.05 | 7.57 | 0.27x |
| 8 | 1.99 | 8.90 | 0.22x |

### sp_mc_value_stack, 1 producer + N consumers

The thread count is the number of consumers; one more thread produces.  Capacity 1024 on both sides; the producer waits while full.

| threads | sp_mc_value_stack | bounded std::vector stack + std::mutex | ratio |
|---:|---:|---:|---:|
| 1 | 2.66 | 4.97 | 0.54x ± |
| 2 | 2.85 | 7.81 | 0.36x |
| 4 | 2.17 | 3.27 | 0.66x |
| 8 | 1.23 | 1.58 | 0.78x |

### spsc_queue, 1 producer + 1 consumer

Both queues hold at most 1024 items; the producer waits while full.

| threads | spsc_queue | bounded std::queue + std::mutex | ratio |
|---:|---:|---:|---:|
| 2 | 19.90 | 4.41 | 4.51x |

### mp_sc_growable_ring_queue, N producers + 1 consumer

The thread count is the number of producers; one more thread consumes.  Throughput counts items moved end to end.  The std queue is unbounded; the ring starts at 1024 and grows.

| threads | mp_sc_growable_ring_queue | std::queue + std::mutex | ratio |
|---:|---:|---:|---:|
| 1 | 69.46 | 6.21 | 11.19x |
| 2 | 11.72 | 5.76 | 2.03x |
| 4 | 10.79 | 4.48 | 2.41x |
| 8 | 9.15 | 3.99 | 2.29x |

### skip_list, 50% find / 25% insert / 25% remove

Keys 0-65535, half present at the start; each thread runs the same operation mix on random keys.

| threads | skip_list | std::map + std::shared_mutex | ratio | std::map + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.45 | 0.59 | 2.46x | 0.60 | 2.42x |
| 2 | 2.20 | 0.22 | 10.00x | 0.27 | 8.15x |
| 4 | 3.63 | 0.20 | 18.15x | 0.25 | 14.52x |
| 8 | 1.40 | 0.20 | 7.00x | 0.28 | 5.00x |

### skip_list, 90% find / 5% insert / 5% remove

Keys 0-65535, half present at the start; each thread runs the same operation mix on random keys.

| threads | skip_list | std::map + std::shared_mutex | ratio | std::map + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.60 | 1.76 | 0.91x | 1.71 | 0.94x ± |
| 2 | 2.92 | 0.39 | 7.49x | 0.63 | 4.63x |
| 4 | 5.29 | 0.36 | 14.69x | 0.60 | 8.82x |
| 8 | 2.96 | 0.36 | 8.22x | 0.66 | 4.48x |

### lockfree_single_arena_resource, cross-thread frees

Each thread allocates 16-256 B blocks and swaps each into a shared mailbox of 64 slots per thread, freeing whatever it swaps out (usually another thread's block).

| threads | lockfree_single_arena_resource | malloc/free | ratio | std::pmr::synchronized_pool_resource | ratio | std::pmr::unsynchronized_pool_resource + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 10.51 | 61.57 | 0.17x | 12.49 | 0.84x | 14.68 | 0.72x |
| 2 | 4.20 | 11.27 | 0.37x | 0.57 | 7.37x | 1.85 | 2.27x |
| 4 | 5.34 | 13.55 | 0.39x | 0.51 | 10.47x | 1.61 | 3.32x |
| 8 | 5.29 | 15.59 | 0.34x | 0.56 | 9.45x | 2.74 | 1.93x |

### lockfree_composite_single_arena_resource, cross-thread frees

Each thread allocates 16-256 B blocks and swaps each into a shared mailbox of 64 slots per thread, freeing whatever it swaps out (usually another thread's block).

| threads | lockfree_composite_single_arena_resource | malloc/free | ratio | std::pmr::synchronized_pool_resource | ratio | std::pmr::unsynchronized_pool_resource + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 18.48 | 61.57 | 0.30x | 12.49 | 1.48x | 14.68 | 1.26x |
| 2 | 6.24 | 11.27 | 0.55x | 0.57 | 10.95x | 1.85 | 3.37x |
| 4 | 7.86 | 13.55 | 0.58x | 0.51 | 15.41x | 1.61 | 4.88x |
| 8 | 7.75 | 15.59 | 0.50x | 0.56 | 13.84x | 2.74 | 2.83x |

### lockfree_bitblock_resource, cross-thread frees

Each thread allocates 16-256 B blocks and swaps each into a shared mailbox of 64 slots per thread, freeing whatever it swaps out (usually another thread's block).

| threads | lockfree_bitblock_resource | malloc/free | ratio | std::pmr::synchronized_pool_resource | ratio | std::pmr::unsynchronized_pool_resource + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 19.33 | 61.57 | 0.31x | 12.49 | 1.55x | 14.68 | 1.32x |
| 2 | 6.32 | 11.27 | 0.56x | 0.57 | 11.09x | 1.85 | 3.42x |
| 4 | 7.73 | 13.55 | 0.57x | 0.51 | 15.16x | 1.61 | 4.80x |
| 8 | 7.71 | 15.59 | 0.49x | 0.56 | 13.77x | 2.74 | 2.81x |

### lockfree_static_arena_resource, cross-thread frees

Each thread allocates 16-256 B blocks and swaps each into a shared mailbox of 64 slots per thread, freeing whatever it swaps out (usually another thread's block).

| threads | lockfree_static_arena_resource | malloc/free | ratio | std::pmr::synchronized_pool_resource | ratio | std::pmr::unsynchronized_pool_resource + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 15.21 | 61.57 | 0.25x | 12.49 | 1.22x ± | 14.68 | 1.04x ± |
| 2 | 4.76 | 11.27 | 0.42x | 0.57 | 8.35x | 1.85 | 2.57x |
| 4 | 2.98 | 13.55 | 0.22x | 0.51 | 5.84x | 1.61 | 1.85x |
| 8 | 2.84 | 15.59 | 0.18x | 0.56 | 5.07x | 2.74 | 1.04x |

### lockfree_single_arena_resource, per-thread churn

Each thread allocates 16-256 B blocks into a window of 64 live slots, freeing a slot's block when the random walk lands on it again.

| threads | lockfree_single_arena_resource | malloc/free | ratio | std::pmr::synchronized_pool_resource | ratio | std::pmr::unsynchronized_pool_resource + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 19.94 | 71.71 | 0.28x | 24.92 | 0.80x ± | 24.26 | 0.82x |
| 2 | 19.96 | 158.11 | 0.13x | 8.41 | 2.37x | 4.97 | 4.02x |
| 4 | 22.11 | 323.09 | 0.07x | 6.53 | 3.39x | 5.17 | 4.28x |
| 8 | 46.82 | 315.38 | 0.15x | 6.41 | 7.30x | 6.91 | 6.78x |

### lockfree_composite_single_arena_resource, per-thread churn

Each thread allocates 16-256 B blocks into a window of 64 live slots, freeing a slot's block when the random walk lands on it again.

| threads | lockfree_composite_single_arena_resource | malloc/free | ratio | std::pmr::synchronized_pool_resource | ratio | std::pmr::unsynchronized_pool_resource + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 29.46 | 71.71 | 0.41x | 24.92 | 1.18x | 24.26 | 1.21x |
| 2 | 69.37 | 158.11 | 0.44x | 8.41 | 8.25x | 4.97 | 13.96x |
| 4 | 133.92 | 323.09 | 0.41x | 6.53 | 20.51x | 5.17 | 25.90x |
| 8 | 127.64 | 315.38 | 0.40x | 6.41 | 19.91x | 6.91 | 18.47x |

### lockfree_bitblock_resource, per-thread churn

Each thread allocates 16-256 B blocks into a window of 64 live slots, freeing a slot's block when the random walk lands on it again.

| threads | lockfree_bitblock_resource | malloc/free | ratio | std::pmr::synchronized_pool_resource | ratio | std::pmr::unsynchronized_pool_resource + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 37.67 | 71.71 | 0.53x | 24.92 | 1.51x | 24.26 | 1.55x |
| 2 | 73.04 | 158.11 | 0.46x | 8.41 | 8.68x | 4.97 | 14.70x |
| 4 | 110.97 | 323.09 | 0.34x | 6.53 | 16.99x | 5.17 | 21.46x |
| 8 | 120.23 | 315.38 | 0.38x | 6.41 | 18.76x | 6.91 | 17.40x |

### lockfree_static_arena_resource, per-thread churn

Each thread allocates 16-256 B blocks into a window of 64 live slots, freeing a slot's block when the random walk lands on it again.

| threads | lockfree_static_arena_resource | malloc/free | ratio | std::pmr::synchronized_pool_resource | ratio | std::pmr::unsynchronized_pool_resource + std::mutex | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 30.34 | 71.71 | 0.42x | 24.92 | 1.22x | 24.26 | 1.25x |
| 2 | 10.84 | 158.11 | 0.07x | 8.41 | 1.29x | 4.97 | 2.18x |
| 4 | 6.06 | 323.09 | 0.02x | 6.53 | 0.93x ± | 5.17 | 1.17x |
| 8 | 5.92 | 315.38 | 0.02x | 6.41 | 0.92x ± | 6.91 | 0.86x ± |

## Reproducing

```bash
make test-performance-comparison     #  not part of `make test-performance`, so CI does not run it (~15 minutes)
#  CMP_REPS=<n> sets the repetitions per cell (default 5); CMP_OPS_PERCENT=<p> scales the operation counts.
#  The report is written to test/performance/reports/LOCKFREE_VS_STD_<timestamp>.md.
```

## Note: a hang in the existing performance suite (not in the library)

Running the whole performance executable once stalled for over 45 minutes in
`MpScGrowableRingQueueExternalComparisonPerformanceTests`. The cause was reproduced and captured with stack traces:
- every producer had finished;
- the only remaining thread was the consumer, spinning in `JiffyMpScQueue::dequeue`
  (`test/performance/external/mp_sc_growable_ring_queue/jiffy_queue.h:130`), waiting for an item it never received.

Jiffy is the vendored third-party MPSC queue used only as a comparison baseline. Its runs in that group are now commented out. `mp_sc_growable_ring_queue` was not
involved. The hang occurs intermittently (1 run in 2 in one trial; the test alone usually completes in about 3 s).
