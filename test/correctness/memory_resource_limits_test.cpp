// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  Boundary cases of the memory resources and heaps: requests too large for the size arithmetic, blocks freed
//      again after being reused, buffers too small for a resource's own bookkeeping, unsupported alignments,
//      and upstream exhaustion.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <__memory_resource/lockfree_bitblock_resource.h>
#include <__memory_resource/lockfree_single_arena_resource.h>
#include <__memory_resource/lockfree_static_arena_resource.h>
#include <__memory_resource/malloc_free_wrapper_memory_resource.h>
#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/memory_heap_resource_adapter.h>
#include <__memory_resource/single_block_resource.h>
#include <__memory_resource/tracking_memory_resource.h>
#include <__memory_resource/__extensions/memory_resource_statistics.h>
#include <single_block_memory_heap>
#include <__platform/cpu_platform_abstractions.h>
#include <stack_memory_heap>

#include "../shared/os_abstractions.h"
#include "../shared/process_isolation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    bool inside(const void *p, const void *buffer, size_t buffer_size)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        const uintptr_t b = reinterpret_cast<uintptr_t>(buffer);

        return (a >= b) && (a < b + buffer_size);
    }

    size_t failure_reports = 0;

    void count_failure(const char *, const char *, const char *, int)
    {
        ++failure_reports;
    }

    using single_arena_type = minstd::pmr::lockfree_single_arena_resource_impl<minstd::platform::default_interrupt_policy,
                                                                               minstd::platform::default_platform_provider,
                                                                               32 * 1024 * 1024,
                                                                               5,
                                                                               128>;
}

TEST_GROUP(MemoryResourceLimitsTests)
{
};

TEST(MemoryResourceLimitsTests, StaticArenaDoesNotLoseTheTailOfALargerBlock)
{
    //  A 64-byte request served from a free 128-byte block used to keep the whole block; deallocate() files it by the
    //      request size, so the other 64 bytes were lost for good.  Now the block is split.
    alignas(64) static char arena[4 * (64 + 4)]; //  four 64-byte blocks and their free-list cells
    minstd::pmr::lockfree_static_arena_resource resource(arena, sizeof(arena));
    CHECK_EQUAL(256u, resource.capacity());

    void *a = resource.allocate(128, 64);
    void *b = resource.allocate(64, 64);
    void *c = resource.allocate(64, 64);
    CHECK(a != nullptr && b != nullptr && c != nullptr);
    POINTERS_EQUAL(nullptr, resource.allocate(64, 64)); //  arena full

    resource.deallocate(a, 128, 64);

    void *p = resource.allocate(64, 64); //  from a's 128-byte block
    CHECK(p != nullptr);
    resource.deallocate(p, 64, 64);

    //  Both halves of a's block are usable.
    void *first = resource.allocate(64, 64);
    void *second = resource.allocate(64, 64);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK(first != second);
    POINTERS_EQUAL(nullptr, resource.allocate(64, 64));
}

TEST(MemoryResourceLimitsTests, StaticArenaLargeBlockIsSplitToTheRequest)
{
    //  A best-fit block from the large bin is split too: the remainder stays allocatable.
    alignas(64) static char arena[256 * (64 + 4)]; //  16 KiB of blocks and their free-list cells
    minstd::pmr::lockfree_static_arena_resource resource(arena, sizeof(arena));
    CHECK_EQUAL(16u * 1024, resource.capacity());

    void *big = resource.allocate(16 * 1024, 64);
    CHECK(big != nullptr);
    resource.deallocate(big, 16 * 1024, 64);

    void *part = resource.allocate(8 * 1024, 64);
    CHECK(part != nullptr);

    size_t small_blocks = 0;
    while (resource.allocate(4096, 64) != nullptr)
    {
        ++small_blocks;
    }

    CHECK_EQUAL(2u, small_blocks); //  the other 8 KiB, as two 4 KiB class blocks
}

TEST(MemoryResourceLimitsTests, SingleArenaTooSmallForItsBookkeepingIsUnusable)
{
    //  The per-CPU arrays alone need more than 64 KiB with the default shard count; the constructor used to lay
    //      them out regardless and write past the caller's buffer.  (Heap buffer: ASan reports an overrun.)
    constexpr size_t SMALL = 64 * 1024;
    CHECK(SMALL < single_arena_type::minimum_block_size());

    char *small_buffer = static_cast<char *>(aligned_alloc(64, SMALL));

    failure_reports = 0;
    auto previous_hook = minstd::set_failure_hook(count_failure);

    {
        single_arena_type resource(small_buffer, SMALL);
        POINTERS_EQUAL(nullptr, resource.allocate(64, 16));
    }

    minstd::set_failure_hook(previous_hook);
    CHECK_EQUAL(1u, failure_reports); //  the constructor reports the undersized block

    free(small_buffer);

    //  minimum_block_size() is enough for a working resource.
    const size_t minimum = single_arena_type::minimum_block_size();
    char *minimum_buffer = static_cast<char *>(aligned_alloc(64, (minimum + 63) & ~size_t{63}));

    {
        single_arena_type resource(minimum_buffer, minimum);
        void *p = resource.allocate(64, 16);
        CHECK(p != nullptr);
        resource.deallocate(p, 64, 16);
    }

    free(minimum_buffer);
}

TEST(MemoryResourceLimitsTests, BitblockResourceRefusesAlignmentAboveSixteen)
{
    //  Allocations are an element boundary + 16, so a 64-byte alignment request used to return a pointer that was
    //      only 16-byte aligned.
    static minstd::pmr::malloc_free_wrapper_memory_resource upstream;
    minstd::pmr::lockfree_bitblock_resource<64, 1024, 4, 8> resource(&upstream, 2);

    POINTERS_EQUAL(nullptr, resource.allocate(64, 64));
    POINTERS_EQUAL(nullptr, resource.allocate(64, 32));

    void *p = resource.allocate(64, 16);
    CHECK(p != nullptr);
    CHECK_EQUAL(0u, reinterpret_cast<uintptr_t>(p) % 16);
    resource.deallocate(p, 64, 16);

    resource.deallocate(nullptr, 64, 16); //  no-op
}

namespace
{
    //  An upstream that fails until enabled, recording the largest request it saw.
    class switchable_upstream : public minstd::pmr::memory_resource
    {
    public:
        bool enabled = false;
        size_t largest_request = 0;

    private:
        minstd::pmr::malloc_free_wrapper_memory_resource malloc_resource_;

        void *do_allocate(size_t bytes, size_t alignment) override
        {
            largest_request = (bytes > largest_request) ? bytes : largest_request;
            return enabled ? malloc_resource_.allocate(bytes, alignment) : nullptr;
        }

        void do_deallocate(void *p, size_t bytes, size_t alignment) override
        {
            malloc_resource_.deallocate(p, bytes, alignment);
        }

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override
        {
            return this == &other;
        }
    };
}

TEST(MemoryResourceLimitsTests, MonotonicBufferSurvivesUpstreamExhaustion)
{
    //  Every failed upstream request used to double the next block size: about 54 failures wrapped it to 0, and the
    //      size loop then never ended.  Before that it never came back down to small requests.
    const bool completed = minstd::pmr::test::runs_to_completion([]
                                                                 {
                                                                     switchable_upstream upstream;
                                                                     minstd::pmr::monotonic_buffer_resource resource(1024, &upstream);

                                                                     for (int i = 0; i < 200; ++i)
                                                                     {
                                                                         if (resource.allocate(16, 16) != nullptr)
                                                                         {
                                                                             _exit(2);
                                                                         }
                                                                     }

                                                                     if (upstream.largest_request > 4096)
                                                                     {
                                                                         _exit(3); //  the next size kept growing on failure
                                                                     }

                                                                     upstream.enabled = true;

                                                                     if (resource.allocate(16, 16) == nullptr)
                                                                     {
                                                                         _exit(4);
                                                                     }
                                                                 });

    CHECK_TRUE(completed);
}

TEST(MemoryResourceLimitsTests, MonotonicBufferRefusesAlignmentsItCannotMask)
{
    //  Alignments the mask arithmetic cannot handle are refused rather than producing garbage.  (Requests near
    //      SIZE_MAX, on the fast path and off it, are covered by AllocatorModelTests.MonotonicBuffer.)

    alignas(16) static char buffer[1024];
    minstd::pmr::monotonic_buffer_resource resource(buffer, sizeof(buffer), nullptr);

    POINTERS_EQUAL(nullptr, resource.allocate(16, 0));
    POINTERS_EQUAL(nullptr, resource.allocate(16, 24));

    void *p = resource.allocate(16, 16);
    CHECK(p != nullptr);
    CHECK(inside(p, buffer, sizeof(buffer)));
}

TEST(MemoryResourceLimitsTests, StackMemoryHeapHonoursItsAlignment)
{
    //  The storage followed the base class's fields with no alignment of its own, so blocks were not aligned as the
    //      ALIGNMENT parameter promised.
    struct holder
    {
        char misalign;
        minstd::stack_memory_heap<4096, 64> heap;
    };

    static holder h;

    CHECK_EQUAL(64u, alignof(minstd::stack_memory_heap<4096, 64>)); //  before: 8, whatever the heap promised

    for (size_t i = 0; i < 4; i++)
    {
        char *block = h.heap.allocate_block<char>(10 + i);
        CHECK(block != nullptr);
        CHECK_EQUAL(0u, reinterpret_cast<uintptr_t>(block) % 64);
    }
}

TEST(MemoryResourceLimitsTests, TrackingResourceIgnoresDeallocateOfNull)
{
    minstd::pmr::malloc_free_wrapper_memory_resource upstream;
    minstd::pmr::tracking_memory_resource resource(&upstream);

    resource.deallocate(nullptr, 64); //  before: bytes_in_use wrapped below zero

    CHECK_EQUAL(0u, resource.bytes_in_use());
    CHECK_EQUAL(0u, resource.deallocation_count());
}

namespace
{
    using small_bin_arena_type = minstd::pmr::lockfree_single_arena_resource_impl<minstd::platform::default_interrupt_policy,
                                                                                  minstd::platform::default_platform_provider,
                                                                                  64 * 1024,
                                                                                  5,
                                                                                  128,
                                                                                  minstd::pmr::extensions::memory_resource_statistics>;
}

TEST(MemoryResourceLimitsTests, SingleArenaBoundaryBehaviour)
{
    constexpr size_t ARENA_BYTES = 4 * 1024 * 1024;
    char *arena = static_cast<char *>(aligned_alloc(4096, ARENA_BYTES));

    {
        small_bin_arena_type resource(arena, ARENA_BYTES);

        //  MAX_ALLOCATION_SIZE is now a size that can succeed (it used to leave no room for the header)

        void *largest = resource.allocate(small_bin_arena_type::MAX_ALLOCATION_SIZE, 16);
        CHECK(largest != nullptr);
        POINTERS_EQUAL(nullptr, resource.allocate(small_bin_arena_type::MAX_ALLOCATION_SIZE + 1, 16));
        resource.deallocate(largest, small_bin_arena_type::MAX_ALLOCATION_SIZE, 16);

        //  deallocate(nullptr) read a header at address -64

        resource.deallocate(nullptr, 64, 16);

        //  The statistics count the size the block was allocated with, not the caller's claim

        void *p = resource.allocate(100, 16);
        CHECK(p != nullptr);
        resource.deallocate(p, 50, 16);
        CHECK_EQUAL(0u, resource.current_bytes_allocated());
    }

    free(arena);
}

TEST(MemoryResourceLimitsTests, StaticArenaKeepsItsFreeListOutsideTheBlocks)
{
    //  The free-bin links live in a table after the blocks (one 4-byte cell per 64-byte block), so a stalled popper
    //      never reads memory a caller owns.  The split must fit the buffer exactly, aligned or not, and a buffer too
    //      small for one block and its cell yields an empty resource.  (Heap buffers: ASan reports any overrun.)

    static const size_t offsets[] = {0, 1, 8, 63};
    static const size_t sizes[] = {0, 10, 64, 67, 68, 69, 136, 1000, 4096};

    for (size_t offset : offsets)
    {
        for (size_t size : sizes)
        {
            char *raw = static_cast<char *>(malloc(size + offset + 1));
            char *start = raw + offset;

            {
                minstd::pmr::lockfree_static_arena_resource resource(start, size);

                const uintptr_t first_block = (reinterpret_cast<uintptr_t>(start) + 63) & ~uintptr_t{63};
                const size_t padding = first_block - reinterpret_cast<uintptr_t>(start);
                const size_t expected_blocks = (size > padding) ? (size - padding) / 68 : 0;

                CHECK_EQUAL(expected_blocks * 64, resource.capacity());

                //  Fill every block, write all of it, then free and refill: the cells must survive all of it

                void *blocks[64];
                size_t count = 0;

                while ((count < 64) && ((blocks[count] = resource.allocate(64, 64)) != nullptr))
                {
                    memset(blocks[count], 0xEE, 64);
                    count++;
                }

                CHECK_EQUAL(expected_blocks < 64 ? expected_blocks : size_t{64}, count);

                for (size_t i = 0; i < count; i++)
                {
                    resource.deallocate(blocks[i], 64, 64);
                }

                for (size_t i = 0; i < count; i++)
                {
                    void *again = resource.allocate(64, 64);
                    CHECK(again != nullptr);
                    memset(again, 0xDD, 64);
                }
            }

            free(raw);
        }
    }
}
