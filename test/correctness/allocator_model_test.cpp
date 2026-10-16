// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//
//  Model-based tests for every memory_resource.  A seeded random sequence of allocations and frees runs against
//      the resource while a shadow model records each live block.  After every step the model checks what any
//      correct resource must guarantee:
//
//      - a block is aligned as requested and does not overlap another live block;
//      - a block's contents survive until it is freed (each block is filled with its own pattern);
//      - nullptr comes back only when the resource is reasonably full (min_fill_at_null of its capacity);
//      - the resource's own statistics, where it keeps them, agree with the model.
//
//  The same harness runs single-threaded on default and small configurations and multi-threaded (each thread with
//      its own model on a shared resource: an overlap between threads shows up as a corrupted pattern).
//

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <random>

#include <__memory_resource/lockfree_bitblock_resource.h>
#include <__memory_resource/lockfree_composite_single_arena_resource.h>
#include <__memory_resource/lockfree_single_arena_resource.h>
#include <__memory_resource/lockfree_static_arena_resource.h>
#include <__memory_resource/malloc_free_wrapper_memory_resource.h>
#include <__memory_resource/memory_heap_resource_adapter.h>
#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/single_block_resource.h>
#include <__memory_resource/tracking_memory_resource.h>
#include <__memory_resource/__extensions/memory_resource_statistics.h>
#include <single_block_memory_heap>
#include <stack_memory_heap>

#include "../shared/os_abstractions.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    struct model_options
    {
        size_t operations = 20000;
        size_t max_size = 2048;         //  request sizes are 1..max_size, skewed towards small
        size_t max_alignment = 16;      //  power of two; requests use 1..max_alignment
        size_t max_live = 512;          //  live blocks the model keeps at most
        size_t capacity = 0;            //  bytes the resource can hand out; 0 disables the null check
        double min_fill_at_null = 0.25; //  a nullptr below this fill of capacity is a violation
        bool check_live_count = false;  //  compare current_allocated() with the model
        size_t charge_overhead = 0;     //  for the null check, a block costs round_up(size + overhead, granule)
        size_t charge_granule = 1;
    };

    struct live_block
    {
        unsigned char *ptr;
        size_t size;
        size_t alignment;
        unsigned char pattern;
    };

    struct model_result
    {
        size_t allocations = 0;
        size_t frees = 0;
        size_t nulls = 0;
        size_t violations = 0;
        char first_violation[256] = "";

        void violation(const char *text)
        {
            if (violations++ == 0)
            {
                snprintf(first_violation, sizeof(first_violation), "%s", text);
            }
        }
    };

    bool pattern_intact(const live_block &block)
    {
        for (size_t i = 0; i < block.size; i++)
        {
            if (block.ptr[i] != static_cast<unsigned char>(block.pattern + i))
            {
                return false;
            }
        }

        return true;
    }

    template <typename resource_type>
    size_t live_count_of(const resource_type &resource)
    {
        if constexpr (requires { resource.current_allocated(); })
        {
            return resource.current_allocated();
        }
        else
        {
            return 0;
        }
    }

    template <typename resource_type>
    model_result run_model(resource_type &resource, uint64_t seed, const model_options &options)
    {
        model_result result;
        char text[256];

        minstd::xoroshiro128_plus_plus engine(seed);

        live_block *live = static_cast<live_block *>(malloc(sizeof(live_block) * options.max_live));
        size_t live_count = 0;
        size_t live_bytes = 0;

        auto charge = [&](size_t size)
        {
            const size_t g = options.charge_granule;
            return ((size + options.charge_overhead + g - 1) / g) * g;
        };

        auto free_one = [&](size_t index)
        {
            live_block &block = live[index];

            if (!pattern_intact(block))
            {
                snprintf(text, sizeof(text), "block %p (%zu bytes) corrupted before free", (void *)block.ptr, block.size);
                result.violation(text);
            }

            resource.deallocate(block.ptr, block.size, block.alignment);
            live_bytes -= charge(block.size);
            live[index] = live[--live_count];
            result.frees++;
        };

        for (size_t op = 0; op < options.operations; op++)
        {
            const bool do_free = (live_count == options.max_live) || ((live_count > 0) && ((engine() % 5) < 2));

            if (do_free)
            {
                free_one(engine() % live_count);
            }
            else
            {
                //  Mostly small requests, sometimes up to the maximum

                const size_t size = ((engine() % 4) != 0) ? 1 + (engine() % minstd::min(options.max_size, (size_t)128))
                                                          : 1 + (engine() % options.max_size);

                size_t alignment = 1;
                while ((alignment < options.max_alignment) && ((engine() % 3) == 0))
                {
                    alignment <<= 1;
                }

                unsigned char *p = static_cast<unsigned char *>(resource.allocate(size, alignment));

                if (p == nullptr)
                {
                    result.nulls++;

                    if ((options.capacity != 0) && ((double)(live_bytes + charge(size)) < options.min_fill_at_null * (double)options.capacity))
                    {
                        snprintf(text, sizeof(text), "nullptr for %zu bytes (align %zu) with only %zu of %zu bytes live",
                                 size, alignment, live_bytes, options.capacity);
                        result.violation(text);
                    }

                    continue;
                }

                result.allocations++;

                if ((reinterpret_cast<uintptr_t>(p) % alignment) != 0)
                {
                    snprintf(text, sizeof(text), "%p not aligned to %zu", (void *)p, alignment);
                    result.violation(text);
                }

                for (size_t i = 0; i < live_count; i++)
                {
                    if ((p < live[i].ptr + live[i].size) && (live[i].ptr < p + size))
                    {
                        snprintf(text, sizeof(text), "%p (%zu bytes) overlaps live %p (%zu bytes)", (void *)p, size, (void *)live[i].ptr, live[i].size);
                        result.violation(text);
                    }
                }

                const unsigned char pattern = static_cast<unsigned char>(engine());

                for (size_t i = 0; i < size; i++)
                {
                    p[i] = static_cast<unsigned char>(pattern + i);
                }

                live[live_count++] = live_block{p, size, alignment, pattern};
                live_bytes += charge(size);
            }

            //  Every so often check every live block, so corruption is caught near its cause

            if ((op % 256) == 0)
            {
                for (size_t i = 0; i < live_count; i++)
                {
                    if (!pattern_intact(live[i]))
                    {
                        snprintf(text, sizeof(text), "live block %p (%zu bytes) corrupted at op %zu", (void *)live[i].ptr, live[i].size, op);
                        result.violation(text);
                        live[i].pattern = live[i].ptr[0]; //  report once
                    }
                }

                if (options.check_live_count && (live_count_of(resource) != live_count))
                {
                    snprintf(text, sizeof(text), "resource counts %zu live blocks, model %zu", live_count_of(resource), live_count);
                    result.violation(text);
                }
            }
        }

        while (live_count > 0)
        {
            free_one(live_count - 1);
        }

        if (options.check_live_count && (live_count_of(resource) != 0))
        {
            snprintf(text, sizeof(text), "resource counts %zu live blocks after everything was freed", live_count_of(resource));
            result.violation(text);
        }

        free(live);

        return result;
    }

    void report(const model_result &result, const char *name)
    {
        if (result.violations != 0)
        {
            char text[400];
            snprintf(text, sizeof(text), "%s: %zu violations, first: %s", name, result.violations, result.first_violation);
            FAIL(text);
        }

        //  The run must actually exercise the resource

        CHECK(result.allocations > 100);
    }

    template <typename resource_type>
    void run_seeds(resource_type &resource, const model_options &options, const char *name, uint64_t first_seed = 1, size_t seeds = 4)
    {
        for (uint64_t seed = first_seed; seed < first_seed + seeds; seed++)
        {
            report(run_model(resource, seed, options), name);
        }
    }

    char *aligned_buffer(size_t bytes)
    {
        return static_cast<char *>(aligned_alloc(4096, (bytes + 4095) & ~(size_t)4095));
    }

    using single_arena_type = minstd::pmr::lockfree_single_arena_resource_impl<minstd::platform::default_interrupt_policy,
                                                                               minstd::platform::default_platform_provider,
                                                                               32 * 1024 * 1024, 5, 128,
                                                                               minstd::pmr::extensions::memory_resource_statistics>;

    using composite_type = minstd::pmr::lockfree_composite_single_arena_resource<1000, 64, 1024, 32, 48, true>;
    using bitblock_type = minstd::pmr::lockfree_bitblock_resource<64, 1024, 4, 8, true>;
}

TEST_GROUP(AllocatorModelTests){};

TEST(AllocatorModelTests, SingleBlockResource)
{
    constexpr size_t BYTES = 1024 * 1024;
    char *buffer = aligned_buffer(BYTES);

    {
        minstd::pmr::single_block_resource resource(buffer, BYTES);

        model_options options;
        options.max_alignment = 64;
        options.capacity = BYTES;
        options.min_fill_at_null = 0.0; //  reuse is per size class (documented), so fullness is not predictable

        run_seeds(resource, options, "single_block_resource");
    }

    free(buffer);
}

TEST(AllocatorModelTests, MonotonicBuffer)
{
    constexpr size_t BYTES = 4 * 1024 * 1024;
    char *buffer = aligned_buffer(BYTES);

    for (uint64_t seed = 1; seed <= 4; seed++)
    {
        minstd::pmr::monotonic_buffer_resource resource(buffer, BYTES, nullptr);

        model_options options;
        options.operations = 4000;
        options.max_alignment = 64;
        options.capacity = BYTES;
        options.min_fill_at_null = 0.0; //  frees are no-ops: the model's live bytes understate what is used

        report(run_model(resource, seed, options), "monotonic_buffer_resource");
    }

    free(buffer);
}

TEST(AllocatorModelTests, StaticArena)
{
    constexpr size_t BYTES = 2 * 1024 * 1024;
    char *buffer = aligned_buffer(BYTES);

    {
        minstd::pmr::lockfree_static_arena_resource resource(buffer, BYTES);

        model_options options;
        options.max_alignment = 64;
        options.capacity = BYTES;
        options.min_fill_at_null = 0.25;

        run_seeds(resource, options, "lockfree_static_arena_resource");
    }

    free(buffer);
}

TEST(AllocatorModelTests, SingleArenaDefaultShards)
{
    constexpr size_t BYTES = 8 * 1024 * 1024;
    char *buffer = aligned_buffer(BYTES);

    {
        single_arena_type resource(buffer, BYTES);

        model_options options;
        options.capacity = BYTES - single_arena_type::minimum_block_size();
        options.min_fill_at_null = 0.25;
        options.check_live_count = true;

        run_seeds(resource, options, "single arena, default shards");
    }

    free(buffer);
}

TEST(AllocatorModelTests, SingleArenaOneShardNearlyFull)
{
    //  A small arena, so the run hits exhaustion and frees into it

    const size_t bytes = single_arena_type::minimum_block_size(1) + 256 * 1024;
    char *buffer = aligned_buffer(bytes);

    {
        single_arena_type resource(buffer, bytes, 1);

        model_options options;
        options.max_size = 4096;
        options.capacity = 256 * 1024;
        options.min_fill_at_null = 0.25;
        options.check_live_count = true;

        run_seeds(resource, options, "single arena, one shard, small");
    }

    free(buffer);
}

TEST(AllocatorModelTests, BitblockResource)
{
    minstd::pmr::malloc_free_wrapper_memory_resource upstream;

    {
        bitblock_type resource(&upstream, 2);

        model_options options;
        options.max_size = 16 * 64 - 16; //  sixteen 64-byte elements less the header
        options.capacity = 8 * 1024 * 64;  //  the block cap: eight blocks of 1024 elements
        options.min_fill_at_null = 0.25;
        options.check_live_count = true;

        run_seeds(resource, options, "lockfree_bitblock_resource");
    }
}

TEST(AllocatorModelTests, BitblockResourceWithExhaustedUpstream)
{
    //  Upstream gives only the two blocks the arenas start with.  With every request on CPU 0, arena 0 fills first;
    //      the resource must then use arena 1's free elements rather than report out of memory at half full.

    class limited_upstream : public minstd::pmr::memory_resource
    {
    private:
        size_t served_ = 0;
        minstd::pmr::malloc_free_wrapper_memory_resource malloc_;

        void *do_allocate(size_t bytes, size_t alignment) override
        {
            return (served_++ < 2) ? malloc_.allocate(bytes, alignment) : nullptr;
        }

        void do_deallocate(void *p, size_t bytes, size_t alignment) override
        {
            malloc_.deallocate(p, bytes, alignment);
        }

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override
        {
            return this == &other;
        }
    };

    minstd::platform::set_test_cpu_id_provider([]() -> uint32_t { return 0u; });

    {
        limited_upstream upstream;
        bitblock_type resource(&upstream, 2);

        model_options options;
        options.max_size = 4 * 64 - 16;  //  small runs, so fragmentation does not decide the result
        options.charge_overhead = 16;     //  each allocation takes whole 64-byte elements, header included
        options.charge_granule = 64;
        options.max_live = 4096;
        options.capacity = 2 * 1024 * 64;
        options.min_fill_at_null = 0.6;  //  without the fallback only arena 0 (half) is usable

        run_seeds(resource, options, "bitblock with exhausted upstream");
    }

    minstd::pmr::test::os_abstractions::install_test_cpu_id_provider();
}

TEST(AllocatorModelTests, CompositeResource)
{
    constexpr size_t BYTES = 16 * 1024 * 1024;
    char *buffer = aligned_buffer(BYTES);

    {
        composite_type resource(buffer, BYTES, 2);

        model_options options;
        options.max_size = 8192;
        options.max_alignment = 16;
        options.capacity = BYTES / 2;
        options.min_fill_at_null = 0.25;
        options.check_live_count = true;

        run_seeds(resource, options, "lockfree_composite_single_arena_resource");
    }

    free(buffer);
}

TEST(AllocatorModelTests, DefaultHeapBehindTheAdapter)
{
    //  The default configuration: a heap with its default alignment, requests with memory_resource's default

    constexpr size_t BYTES = 256 * 1024;
    char *buffer = aligned_buffer(BYTES + 8);

    {
        minstd::single_block_memory_heap heap(buffer + 8, BYTES); //  deliberately not 16-aligned
        minstd::pmr::memory_heap_resource_adapter resource(heap);

        model_options options;
        options.max_size = 512;
        options.max_alignment = 16;
        options.capacity = BYTES;
        options.min_fill_at_null = 0.1; //  the heap reuses only blocks of a fitting size

        run_seeds(resource, options, "single_block_memory_heap + adapter");
    }

    free(buffer);
}

TEST(AllocatorModelTests, StackHeapBehindTheAdapter)
{
    static minstd::stack_memory_heap<128 * 1024> heap;
    minstd::pmr::memory_heap_resource_adapter resource(heap);

    model_options options;
    options.max_size = 256;
    options.max_alignment = 16;
    options.capacity = 128 * 1024;
    options.min_fill_at_null = 0.1;

    report(run_model(resource, 7, options), "stack_memory_heap + adapter");
}

TEST(AllocatorModelTests, TrackingOverMalloc)
{
    minstd::pmr::malloc_free_wrapper_memory_resource upstream;
    minstd::pmr::tracking_memory_resource resource(&upstream);

    model_options options;
    options.max_alignment = 64;

    run_seeds(resource, options, "tracking over malloc");

    CHECK_EQUAL(0u, resource.bytes_in_use());
    CHECK_EQUAL(resource.allocation_count(), resource.deallocation_count());
}

//
//  Multi-threaded: four threads, each with its own model, on one shared resource
//

namespace
{
    constexpr size_t MODEL_THREADS = 4;

    template <typename resource_type>
    struct thread_model_args
    {
        resource_type *resource;
        uint64_t seed;
        model_options options;
        model_result result;
    };

    template <typename resource_type>
    void *thread_model_main(void *raw)
    {
        auto *args = static_cast<thread_model_args<resource_type> *>(raw);
        args->result = run_model(*args->resource, args->seed, args->options);
        return nullptr;
    }

    template <typename resource_type>
    void run_threads(resource_type &resource, model_options options, const char *name)
    {
        options.capacity = 0; //  fullness depends on the other threads
        options.check_live_count = false;
        options.operations = 20000;

        thread_model_args<resource_type> args[MODEL_THREADS];
        pthread_t threads[MODEL_THREADS];

        for (size_t i = 0; i < MODEL_THREADS; i++)
        {
            args[i].resource = &resource;
            args[i].seed = 100 + i;
            args[i].options = options;
            CHECK_EQUAL(0, pthread_create(&threads[i], nullptr, thread_model_main<resource_type>, &args[i]));
        }

        for (size_t i = 0; i < MODEL_THREADS; i++)
        {
            CHECK_EQUAL(0, pthread_join(threads[i], nullptr));
        }

        for (size_t i = 0; i < MODEL_THREADS; i++)
        {
            report(args[i].result, name); //  CppUTest checks only on the main thread
        }
    }
}

TEST(AllocatorModelTests, ConcurrentSingleArena)
{
    constexpr size_t BYTES = 16 * 1024 * 1024;
    char *buffer = aligned_buffer(BYTES);

    {
        single_arena_type resource(buffer, BYTES);
        model_options options;
        run_threads(resource, options, "concurrent single arena");
        CHECK_EQUAL(0u, resource.current_allocated());
    }

    free(buffer);
}

TEST(AllocatorModelTests, ConcurrentStaticArena)
{
    constexpr size_t BYTES = 8 * 1024 * 1024;
    char *buffer = aligned_buffer(BYTES);

    {
        minstd::pmr::lockfree_static_arena_resource resource(buffer, BYTES);
        model_options options;
        options.max_alignment = 64;
        run_threads(resource, options, "concurrent static arena");
    }

    free(buffer);
}

TEST(AllocatorModelTests, ConcurrentBitblock)
{
    minstd::pmr::malloc_free_wrapper_memory_resource upstream;

    {
        bitblock_type resource(&upstream, 4);
        model_options options;
        options.max_size = 16 * 64 - 16;
        run_threads(resource, options, "concurrent bitblock");
        CHECK_EQUAL(0u, resource.current_allocated());
    }
}

TEST(AllocatorModelTests, ConcurrentComposite)
{
    constexpr size_t BYTES = 32 * 1024 * 1024;
    char *buffer = aligned_buffer(BYTES);

    {
        composite_type resource(buffer, BYTES, 4);
        model_options options;
        options.max_size = 8192;
        run_threads(resource, options, "concurrent composite");
        CHECK_EQUAL(0u, resource.current_allocated());
    }

    free(buffer);
}
