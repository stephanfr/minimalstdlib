# `lockfree_single_arena_resource`: performance analysis

This is an analysis only; no allocator code has been changed.

**Measured state:**
- Single-CPU throughput is about 0.3x glibc `malloc`/`free`.
- Throughput barely grows with more CPUs.

**What a prototype showed is possible**, without giving up lock-freedom or per-CPU sharding:
- about 0.7-1.0x `malloc` on a single CPU;
- about 4.7x the current throughput at 4 CPUs.

The prototype diff is in the appendix.

## Setup

- **Scratch benchmark:** the per-CPU churn workload from `LockfreeVsStdComparisonPerformanceTests`. Each CPU runs a
  window of 64 live blocks of 16-256 B and frees a slot's block when the random walk lands on it again.
- **Instrumentation:** cycle counts from `rdtsc` around each `allocate` and `deallocate`, and `callgrind` for
  instruction counts.
- **Build:** `__MINIMAL_STD_TEST__`, so the CPU id is `sched_getcpu()`, cached for 500 calls.
- **Host:** x86-64 VM, 4 vCPUs. There are no hardware performance counters, so cache misses could not be counted
  directly.
- **Reading the numbers:** throughput is in Mops/s, and malloc runs in the same harness. Run-to-run noise is about
  ±15%.

## Where the time goes (single CPU)

- **Cost per operation:** about 300 cycles for `allocate` and 250 for `deallocate`, against 45-50 each for glibc.
- **Path mix:** 97-99% of allocations reuse a freed block, 1-3% carve from the frontier, and a maintenance window
  runs once per 128 frees.
- **Atomic read-modify-writes:** about 6 per allocate/free pair:
  - bin pop;
  - claim of the block (AVAILABLE → LOCKED);
  - lock on free (IN_USE → LOCKED);
  - pending-count increment;
  - pending push;
  - re-push into a free bin during maintenance.
- **Size-class lookup:** `free_block_bin_index` is a binary search over a few hundred bins, about a third of the
  allocator's instructions. It runs on every allocation and again for every block a maintenance window re-files.
- **Divides on the fast path:**
  - `cpu_shard_index()` computes `get_cpu_id() % cpu_shards_`, a 64-bit divide by a runtime value, on every
    allocate and every free;
  - the alignment check `DEFAULT_ALIGNMENT % alignment` divides on every allocate.
- **Maintenance windows:** about 137 cycles per freed block, re-pushing each block into the bins.

## Multiple CPUs

**One shared instance does not scale.** Four CPUs reach 10-14 Mops/s on one instance, against 33-36 Mops/s with a
separate instance each. The cost per operation roughly triples, to about 850 cycles for allocate and 950 for free. The
cause is cross-shard scanning:
- When a CPU's own bin for a size is empty, which happens often between maintenance windows, the search reads and
  pops other CPUs' bin heads.
- About 12% of allocations steal from another shard, and about 0.5-1 of other cores' bin heads are read per
  allocation.
- Each such read forces the owning CPU's next push or pop to fetch the cache line back.
- Never scanning other shards (not a viable policy, only a bound) lifts 4 CPUs from about 10.7 to 18.6 Mops/s.

**Ruled out by experiment:**
- the resource object's layout: read-only fields were separated from atomics, each atomic on its own 128-byte line;
- false sharing of per-CPU arrays, including 128-byte spacing;
- interleaved metadata records;
- the frontier and frontier rollback;
- the metadata trim;
- thread placement or pinning.

## Opportunities

| # | Change | Behaviour change | Single CPU | 4 CPUs | Notes |
|---|--------|------------------|------------|--------|-------|
| 1 | Size-class lookup table (`constexpr`, 1024 entries, sizes up to 64 KiB) instead of binary search | none | | | |
| 2 | No divides on the fast path: `cpu_id < shards ? cpu_id : cpu_id % shards`; power-of-two alignment check | none | 1+2: about 1.3x (11 vs 8.5) | 1+2: 14.7 vs 12.5 | no model change needed |
| 3 | Per-CPU cache of recently freed small blocks (below) | new block state | 1+2+3: about 2.5x (20-24 vs 8.5), about 0.7-1.0x malloc | 1+2+3: 58.8 vs 12.5 (4.7x); 8 CPUs 69.0 vs 21.4 | cross-CPU frees 3-4x (21.8 / 11.7 / 14.2 vs 5.5 / 4.1 / 4.1 at 1 / 4 / 8) |
| 4 | Search own shard, then the frontier, then other shards | allocation order | about +15-30% | about +30-60% | costs arena memory under multi-CPU churn (+14% at 4 CPUs, +37% at 8); largely superseded by 3 |

## The per-CPU cache (opportunity 3)

**Data structure.** For each CPU shard, a 64-byte-aligned `cpu_cache` holds, for each of the 16 smallest size
classes (blocks up to 1 KiB including the header), a singly linked list of metadata indices and a count. A list holds
at most 32 blocks. The lists link through `next_free_block_index_`, with atomic relaxed accesses, as the tagged stack
already does.

**Free fast path.** It runs after the existing validation (index in range, state IN_USE, pointer matches):
1. Take this CPU's cache try-flag with a CAS.
2. If the class's list has room, CAS the block's state from IN_USE to a new state, CPU_CACHED, with the version
   advanced. This keeps double-free detection: a second free sees CPU_CACHED and is rejected.
3. Push the block onto the list and release the flag.
4. If the flag is busy or the list is full, use the existing path.

**Allocate fast path.**
1. Take the try-flag.
2. Pop a block of the request's size class and release the flag.
3. Store IN_USE with a release store, then write the requested size, alignment and shard.
4. On a miss, use the existing path.

**Why it stays lock-free.** The flag is never waited on. If it is busy, because an interrupt on the same CPU arrived
inside the window or the holder was preempted, the operation takes the existing lock-free path. Removing the flag,
which is only safe if each CPU id is used by one context at a time, bought no measurable gain, so the safe version
costs nothing.

**What the prototype leaves out**, all of which a real implementation needs:
- **Draining on out-of-memory:** before failing, take each CPU's cache try-flag in turn and send its blocks through
  the normal free path. Blocks held by a stalled flag holder can be missed, bounded by 32 blocks per class per CPU;
  that needs documenting.
- **Bounds:** a per-CPU byte cap on cached memory, and a template parameter for depth and class count.
- **Other state readers:** allocation queries must handle CPU_CACHED.
- **The model:** `tlaplus/lockfree_allocator/LockfreeSingleArena.tla` needs the CPU_CACHED state, the try-flag and
  the out-of-memory drain. CPU_CACHED blocks must never be claimed by search, rolled back by the frontier walk, or
  reach the bins.
- **Tests:** double free across CPUs, the drain on out-of-memory, the busy-flag fallback (simulated interrupt), and
  ASan, TSan and stress runs.

## Appendix: prototype diff (opportunities 1-3, without the out-of-memory drain)

```diff
--- a/include/__memory_resource/lockfree_single_arena_resource.h
+++ b/include/__memory_resource/lockfree_single_arena_resource.h
@@ -173,7 +173,8 @@
                 METADATA_AVAILABLE,
                 LOCKED,
                 FRONTIER_RECLAIM_IN_PROGRESS,
-                FRONTIER_RECLAIMED
+                FRONTIER_RECLAIMED,
+                CPU_CACHED
             };
 
             static constexpr uint32_t NULL_INDEX = UINT32_MAX;
@@ -221,6 +222,13 @@
                 }
                 current_ptr += cpu_shards_ * sizeof(pending_shard);
 
+                cpu_caches_ = reinterpret_cast<cpu_cache *>(current_ptr);
+                for (size_t i = 0; i < cpu_shards_; ++i)
+                {
+                    new (&cpu_caches_[i]) cpu_cache();
+                }
+                current_ptr += cpu_shards_ * sizeof(cpu_cache);
+
                 //  Allocate free_block_bins_ array (NUM_FREE_BLOCK_BINS * cpu_shards_ elements)
                 free_block_bins_ = reinterpret_cast<free_block_bin *>(current_ptr);
                 current_ptr += NUM_FREE_BLOCK_BINS * cpu_shards_ * sizeof(free_block_bin);
@@ -507,6 +515,25 @@
 
             static constexpr array<size_t, NUM_FREE_BLOCK_BINS> FREE_BLOCK_BIN_SIZES = build_free_block_bin_sizes();
 
+            static constexpr size_t SMALL_BIN_LOOKUP_UNITS = 1024;
+
+            static constexpr array<uint16_t, SMALL_BIN_LOOKUP_UNITS + 1> build_small_bin_lookup()
+            {
+                array<uint16_t, SMALL_BIN_LOOKUP_UNITS + 1> lookup{};
+                size_t bin = 0;
+                for (size_t units = 0; units <= SMALL_BIN_LOOKUP_UNITS; ++units)
+                {
+                    while (FREE_BLOCK_BIN_SIZES[bin] < units * DEFAULT_ALIGNMENT)
+                    {
+                        ++bin;
+                    }
+                    lookup[units] = static_cast<uint16_t>(bin);
+                }
+                return lookup;
+            }
+
+            static constexpr array<uint16_t, SMALL_BIN_LOOKUP_UNITS + 1> SMALL_BIN_LOOKUP = build_small_bin_lookup();
+
             static_assert(NUM_NON_SENTINEL_FREE_BLOCK_BINS >= 2,
                           "Uniform bin policy must generate at least two non-sentinel bins");
 
@@ -534,6 +561,37 @@
 
             pending_shard *pending_shards_;
 
+            static constexpr size_t CPU_CACHE_CLASSES = 16;
+            static constexpr uint32_t CPU_CACHE_DEPTH = 32;
+
+            struct alignas(64) cpu_cache
+            {
+                atomic<uint32_t> busy_{0};
+                uint32_t count_[CPU_CACHE_CLASSES] = {};
+                uint32_t head_[CPU_CACHE_CLASSES] = {};
+            };
+
+            cpu_cache *cpu_caches_ = nullptr;
+
+            block_metadata *cpu_cache_pop(size_t shard, uint64_t bin)
+            {
+                cpu_cache &cache = cpu_caches_[shard];
+                uint32_t expected = 0;
+                if (!cache.busy_.compare_exchange_strong(expected, 1, memory_order_acquire, memory_order_relaxed))
+                {
+                    return nullptr;
+                }
+                block_metadata *metadata = nullptr;
+                if (cache.count_[bin] != 0)
+                {
+                    metadata = index_to_metadata(cache.head_[bin]);
+                    cache.head_[bin] = __atomic_load_n(&metadata->next_free_block_index_, __ATOMIC_RELAXED);
+                    --cache.count_[bin];
+                }
+                cache.busy_.store(0, memory_order_release);
+                return metadata;
+            }
+
             //  Per-CPU free block bins - dynamically allocated from the managed block.
             free_block_bin *free_block_bins_;
             size_t cpu_shards_;
@@ -576,7 +634,8 @@
 
             size_t cpu_shard_index() const
             {
-                return static_cast<size_t>(platform_provider_type::get_cpu_id()) % cpu_shards_;
+                const size_t cpu_id = static_cast<size_t>(platform_provider_type::get_cpu_id());
+                return (cpu_id < cpu_shards_) ? cpu_id : (cpu_id % cpu_shards_);
             }
 
             const block_header &as_block_header(void *block) const
@@ -643,7 +702,13 @@
                     return NUM_FREE_BLOCK_BINS - 1;
                 }
 
-                size_t lo = 0;
+                const size_t units = (bytes + DEFAULT_ALIGNMENT - 1) / DEFAULT_ALIGNMENT;
+                if (units <= SMALL_BIN_LOOKUP_UNITS)
+                {
+                    return SMALL_BIN_LOOKUP[units];
+                }
+
+                size_t lo = SMALL_BIN_LOOKUP[SMALL_BIN_LOOKUP_UNITS];
                 size_t hi = NUM_FREE_BLOCK_BINS - 1;
 
                 while (lo < hi)
@@ -1080,7 +1145,7 @@
                 //  We cannot throw and exception which is what the standard requires if we cannot allocate the memory
                 //      of either the desired size or alignment, so we will just return nullptr.
 
-                if ((bytes == 0) || (DEFAULT_ALIGNMENT % alignment != 0))
+                if ((bytes == 0) || (alignment == 0) || (alignment > DEFAULT_ALIGNMENT) || ((alignment & (alignment - 1)) != 0))
                 {
                     return nullptr;
                 }
@@ -1096,6 +1161,24 @@
                 }
 
                 auto shard = cpu_shard_index();
+
+                {
+                    const uint64_t cache_bin = free_block_bin_index(total_size);
+                    if (cache_bin < CPU_CACHE_CLASSES)
+                    {
+                        if (block_metadata *cached = cpu_cache_pop(shard, cache_bin))
+                        {
+                            const uint64_t cached_state = cached->block_state_.load(memory_order_relaxed);
+                            block_header *cached_block = block_state_ptr::unpack_ptr(cached_state, block_);
+                            cached->block_state_.store(block_state_ptr::with_state(cached_state, IN_USE), memory_order_release);
+                            cached->requested_size_.store(static_cast<uint32_t>(bytes), memory_order_release);
+                            cached->alignment_ = static_cast<uint8_t>(alignment);
+                            cached->original_shard_ = static_cast<uint8_t>(shard);
+                            return reinterpret_cast<uint8_t *>(cached_block) + ALLOCATION_HEADER_SIZE;
+                        }
+                    }
+                }
+
                 maybe_open_maintenance_window(shard, false);
 
                 auto free_block_bin = free_block_bin_index(total_size);
@@ -1281,6 +1364,33 @@
                     // Build expected and desired values for CAS
                     // Expected: current pointer + IN_USE + current version
                     // Desired: same pointer + LOCKED + incremented version
+                    {
+                        const uint64_t cache_bin = free_block_bin_index(block_to_deallocate->total_size_);
+                        if (cache_bin < CPU_CACHE_CLASSES)
+                        {
+                            cpu_cache &cache = cpu_caches_[cpu_shard_index()];
+                            uint32_t expected = 0;
+                            if (cache.busy_.compare_exchange_strong(expected, 1, memory_order_acquire, memory_order_relaxed))
+                            {
+                                if (cache.count_[cache_bin] < CPU_CACHE_DEPTH)
+                                {
+                                    const uint64_t cached_state = block_state_ptr::with_state_and_increment_version(current_block_state, CPU_CACHED);
+                                    if (!block_to_deallocate->block_state_.compare_exchange_strong(current_block_state, cached_state, memory_order_acq_rel, memory_order_acquire))
+                                    {
+                                        cache.busy_.store(0, memory_order_release);
+                                        return false;
+                                    }
+                                    __atomic_store_n(&block_to_deallocate->next_free_block_index_, cache.head_[cache_bin], __ATOMIC_RELAXED);
+                                    cache.head_[cache_bin] = metadata_to_index(block_to_deallocate);
+                                    ++cache.count_[cache_bin];
+                                    cache.busy_.store(0, memory_order_release);
+                                    return true;
+                                }
+                                cache.busy_.store(0, memory_order_release);
+                            }
+                        }
+                    }
+
                     locked_block_state = block_state_ptr::with_state_and_increment_version(current_block_state, LOCKED);
 
                     if (!block_to_deallocate->block_state_.compare_exchange_strong(current_block_state, locked_block_state, memory_order_acq_rel, memory_order_acquire))
```
