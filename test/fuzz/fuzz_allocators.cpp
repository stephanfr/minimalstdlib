// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  Fuzz the memory resources: the input drives a sequence of allocations (size, alignment) and frees on each
//      resource, with a shadow model checking alignment, overlap with live blocks and that every live block's
//      contents survive.  A violation aborts; ASan catches out-of-bounds writes by the resources themselves.

#include <minstdconfig.h>

#include <__memory_resource/lockfree_bitblock_resource.h>
#include <__memory_resource/lockfree_composite_single_arena_resource.h>
#include <__memory_resource/lockfree_single_arena_resource.h>
#include <__memory_resource/lockfree_static_arena_resource.h>
#include <__memory_resource/malloc_free_wrapper_memory_resource.h>
#include <__memory_resource/memory_heap_resource_adapter.h>
#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/single_block_resource.h>
#include <single_block_memory_heap>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace
{
    struct live_block
    {
        unsigned char *ptr;
        size_t size;
        size_t alignment;
        unsigned char pattern;
    };

    bool intact(const live_block &b)
    {
        for (size_t i = 0; i < b.size; i++)
        {
            if (b.ptr[i] != static_cast<unsigned char>(b.pattern + i)) return false;
        }
        return true;
    }

    template <typename resource_type>
    void drive(resource_type &resource, const uint8_t *data, size_t size, size_t max_size, size_t max_alignment)
    {
        constexpr size_t MAX_LIVE = 256;
        live_block live[MAX_LIVE];
        size_t count = 0;
        size_t at = 0;

        auto next = [&]() -> uint8_t { return (at < size) ? data[at++] : 0; };

        while (at < size)
        {
            const uint8_t op = next();

            if (((op & 3) == 0) && (count > 0))
            {
                const size_t index = next() % count;
                if (!intact(live[index])) abort();
                resource.deallocate(live[index].ptr, live[index].size, live[index].alignment);
                live[index] = live[--count];
                continue;
            }

            if (count == MAX_LIVE)
            {
                continue;
            }

            const size_t bytes = 1 + ((static_cast<size_t>(next()) << 8 | next()) % max_size);
            size_t alignment = static_cast<size_t>(1) << (next() % 8);
            alignment = (alignment > max_alignment) ? max_alignment : alignment;

            unsigned char *p = static_cast<unsigned char *>(resource.allocate(bytes, alignment));

            if (p == nullptr)
            {
                continue;
            }

            if ((reinterpret_cast<uintptr_t>(p) % alignment) != 0) abort();

            for (size_t i = 0; i < count; i++)
            {
                if ((p < live[i].ptr + live[i].size) && (live[i].ptr < p + bytes)) abort();
            }

            const unsigned char pattern = op;
            for (size_t i = 0; i < bytes; i++) p[i] = static_cast<unsigned char>(pattern + i);

            live[count++] = live_block{p, bytes, alignment, pattern};
        }

        while (count > 0)
        {
            --count;
            if (!intact(live[count])) abort();
            resource.deallocate(live[count].ptr, live[count].size, live[count].alignment);
        }
    }

    using single_arena_type = minstd::pmr::lockfree_single_arena_resource_impl<minstd::platform::default_interrupt_policy,
                                                                               minstd::platform::default_platform_provider,
                                                                               64 * 1024, 5, 16>;

    char *aligned_buffer(size_t bytes)
    {
        return static_cast<char *>(aligned_alloc(4096, (bytes + 4095) & ~(size_t)4095));
    }

    //  Built once and reused across inputs (every input frees what it allocated)

    struct resources
    {
        char *static_buffer = aligned_buffer(256 * 1024);
        char *single_buffer = aligned_buffer(single_arena_type::minimum_block_size(1) + 256 * 1024);
        char *composite_buffer = aligned_buffer(4 * 1024 * 1024);
        char *block_buffer = aligned_buffer(256 * 1024);
        char *heap_buffer = aligned_buffer(256 * 1024 + 8);

        minstd::pmr::lockfree_static_arena_resource static_arena{static_buffer, 256 * 1024};
        single_arena_type single_arena{single_buffer, single_arena_type::minimum_block_size(1) + 256 * 1024, 1};
        minstd::pmr::lockfree_composite_single_arena_resource<1000, 64, 1024, 32, 48, false> composite{composite_buffer, 4 * 1024 * 1024, 2};
        minstd::pmr::malloc_free_wrapper_memory_resource malloc_resource;
        minstd::pmr::lockfree_bitblock_resource<64, 1024, 4, 8> bitblock{&malloc_resource, 2};
        minstd::pmr::single_block_resource single_block{block_buffer, 256 * 1024};
        minstd::single_block_memory_heap heap{heap_buffer + 8, 256 * 1024};
        minstd::pmr::memory_heap_resource_adapter adapter{heap};
    };

    resources &get()
    {
        static resources r;
        return r;
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    resources &r = get();

    drive(r.static_arena, data, size, 16384, 128);
    drive(r.single_arena, data, size, 8192, 16);
    drive(r.composite, data, size, 8192, 16);
    drive(r.bitblock, data, size, 16 * 64 - 16, 16);
    drive(r.single_block, data, size, 8192, 128);
    drive(r.adapter, data, size, 2048, 16);
    drive(r.malloc_resource, data, size, 8192, 128);

    //  The monotonic resource cannot reuse memory, so it gets a fresh buffer per input

    static char *monotonic_buffer = aligned_buffer(64 * 1024);
    minstd::pmr::monotonic_buffer_resource monotonic(monotonic_buffer, 64 * 1024, nullptr);
    drive(monotonic, data, size, 4096, 128);

    return 0;
}
