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
#include <stack_memory_heap>

#include "../shared/process_isolation.h"

#include <stdint.h>
#include <stdlib.h>

namespace
{
    constexpr size_t HUGE_REQUESTS[] = {SIZE_MAX, SIZE_MAX - 63, SIZE_MAX - 64, SIZE_MAX - 4096, SIZE_MAX / 2 + 1};

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

TEST(MemoryResourceLimitsTests, StaticArenaRejectsRequestsThatWouldWrap)
{
    //  The size was rounded with (size + 63) & ~63 and the end found by pointer addition: both wrapped, so a huge
    //      request returned a block and could move the bump pointer below the arena.
    alignas(64) static char arena[64 * 1024];
    minstd::pmr::lockfree_static_arena_resource resource(arena, sizeof(arena));

    for (size_t bytes : HUGE_REQUESTS)
    {
        POINTERS_EQUAL(nullptr, resource.allocate(bytes, 64));
    }

    void *p = resource.allocate(128, 64);
    CHECK(p != nullptr);
    CHECK(inside(p, arena, sizeof(arena)));
}

TEST(MemoryResourceLimitsTests, StaticArenaDoesNotLoseTheTailOfALargerBlock)
{
    //  A 64-byte request served from a free 128-byte block used to keep the whole block; deallocate() files it by the
    //      request size, so the other 64 bytes were lost for good.  Now the block is split.
    alignas(64) static char arena[256];
    minstd::pmr::lockfree_static_arena_resource resource(arena, sizeof(arena));

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
    alignas(64) static char arena[16 * 1024];
    minstd::pmr::lockfree_static_arena_resource resource(arena, sizeof(arena));

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

TEST(MemoryResourceLimitsTests, SingleArenaRejectsRequestsThatWouldWrap)
{
    //  bytes + header used to wrap to a tiny size, so a huge request returned a header-only block.
    char *arena = static_cast<char *>(aligned_alloc(4096, 4 * 1024 * 1024));

    {
        single_arena_type resource(arena, 4 * 1024 * 1024);

        for (size_t bytes : HUGE_REQUESTS)
        {
            POINTERS_EQUAL(nullptr, resource.allocate(bytes, 16));
        }

        void *p = resource.allocate(100, 16);
        CHECK(p != nullptr);
        resource.deallocate(p, 100, 16);
    }

    free(arena);
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

TEST(MemoryResourceLimitsTests, SingleBlockResourceRejectsRequestsThatWouldWrap)
{
    //  The end of the new block was found by pointer addition, which wrapped: the next allocation landed below the
    //      buffer.
    alignas(64) static char block[64 * 1024];
    minstd::pmr::single_block_resource resource(block, sizeof(block));

    void *first = resource.allocate(64);
    CHECK(first != nullptr);

    for (size_t bytes : HUGE_REQUESTS)
    {
        POINTERS_EQUAL(nullptr, resource.allocate(bytes));
    }

    void *p = resource.allocate(64);
    CHECK(p != nullptr);
    CHECK(inside(p, block, sizeof(block)));
}

TEST(MemoryResourceLimitsTests, SingleBlockResourceBlockFreedAgainAfterReuse)
{
    //  A block reused from a deallocated list stays linked on it until a later scan unlinks it.  Freeing it again
    //      before that pushed it a second time, linking it to itself, and the next scan of its bin never ended.
    const bool completed = minstd::pmr::test::runs_to_completion([]
                                                                 {
                                                                     alignas(64) static char block[64 * 1024];
                                                                     minstd::pmr::single_block_resource resource(block, sizeof(block));

                                                                     void *a = resource.allocate(100);
                                                                     resource.allocate(100);
                                                                     resource.deallocate(a, 100);

                                                                     void *p = resource.allocate(64); //  reuses a
                                                                     if (p != a)
                                                                     {
                                                                         _exit(2);
                                                                     }
                                                                     resource.deallocate(p, 64);

                                                                     void *q = resource.allocate(900); //  scans a's bin
                                                                     if ((q == nullptr) || (q == a))
                                                                     {
                                                                         _exit(3);
                                                                     }

                                                                     void *r = resource.allocate(64); //  a is still reusable
                                                                     if (r != a)
                                                                     {
                                                                         _exit(4);
                                                                     }
                                                                 });

    CHECK_TRUE(completed);
}

TEST(MemoryResourceLimitsTests, SingleBlockMemoryHeapRejectsRequestsThatWouldWrap)
{
    //  element size * count, the alignment rounding and bytes_allocated_ + size all wrapped: a huge request
    //      returned a header-only block or moved the end of the heap backwards.
    alignas(64) static char heap_memory[64 * 1024];
    minstd::single_block_memory_heap heap(heap_memory, sizeof(heap_memory), 16);

    char *first = heap.allocate_block<char>(100);
    CHECK(first != nullptr);

    POINTERS_EQUAL(nullptr, heap.allocate_block<char>(SIZE_MAX));
    POINTERS_EQUAL(nullptr, heap.allocate_block<char>(SIZE_MAX - 100));
    POINTERS_EQUAL(nullptr, heap.allocate_block<uint64_t>(SIZE_MAX / 4));

    char *p = heap.allocate_block<char>(100);
    CHECK(p != nullptr);
    CHECK(inside(p, heap_memory, sizeof(heap_memory)));
    CHECK(p > first);
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

TEST(MemoryResourceLimitsTests, SingleBlockResourceNearlyFullRejectsRequestsThatWouldWrap)
{
    //  The remaining-space check subtracted the alignment from the space left; with under 16 bytes left that wrapped,
    //      and a near-SIZE_MAX request was granted and moved the bump pointer backwards.  Try every tail size.
    alignas(64) static char block[512];

    for (size_t size = 96; size <= sizeof(block); size++)
    {
        minstd::pmr::single_block_resource resource(block, size);

        while (resource.allocate(16) != nullptr)
        {
        }

        for (size_t k = 0; k < 256; k++)
        {
            POINTERS_EQUAL(nullptr, resource.allocate(SIZE_MAX - k));
        }
    }
}

TEST(MemoryResourceLimitsTests, SingleBlockResourceReusePathRejectsRequestsThatWouldWrap)
{
    //  With enough frees the resource searches its free lists first; bytes + header wrapped there and matched a
    //      small freed block.
    alignas(64) static char block[64 * 1024];
    minstd::pmr::single_block_resource resource(block, sizeof(block));

    void *blocks[8];

    for (auto &b : blocks)
    {
        b = resource.allocate(64);
        CHECK(b != nullptr);
    }

    for (auto &b : blocks)
    {
        resource.deallocate(b, 64);
    }

    for (size_t k = 0; k < 128; k++)
    {
        POINTERS_EQUAL(nullptr, resource.allocate(SIZE_MAX - k));
    }
}

TEST(MemoryResourceLimitsTests, MonotonicBufferFastPathRejectsRequestsThatWouldWrap)
{
    //  padding + bytes wrapped on the fast path, so a near-SIZE_MAX request was granted from the current buffer.
    alignas(16) static char buffer[1024];
    minstd::pmr::monotonic_buffer_resource resource(buffer, sizeof(buffer), nullptr);

    CHECK(resource.allocate(8, 8) != nullptr); //  the next free byte is now 8 mod 16

    for (size_t k = 0; k < 32; k++)
    {
        POINTERS_EQUAL(nullptr, resource.allocate(SIZE_MAX - k, 16));
    }

    //  Alignments the mask arithmetic cannot handle are refused rather than producing garbage

    POINTERS_EQUAL(nullptr, resource.allocate(16, 0));
    POINTERS_EQUAL(nullptr, resource.allocate(16, 24));

    void *p = resource.allocate(16, 16);
    CHECK(p != nullptr);
    CHECK(inside(p, buffer, sizeof(buffer)));
}

TEST(MemoryResourceLimitsTests, HeapAdapterRejectsRequestsThatWouldWrap)
{
    //  Rounding bytes up to whole elements wrapped to zero elements, and the heap returned a header-only block.
    alignas(64) static char heap_buffer[16 * 1024];
    minstd::single_block_memory_heap heap(heap_buffer, sizeof(heap_buffer), 16);
    minstd::pmr::memory_heap_resource_adapter resource(heap);

    for (size_t k = 0; k < 16; k++)
    {
        POINTERS_EQUAL(nullptr, resource.allocate(SIZE_MAX - k, 16));
    }

    CHECK_EQUAL(0, heap.bytes_in_use());
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
