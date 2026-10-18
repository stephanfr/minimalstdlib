// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>
#include <minstdconfig.h>

#include <__memory_resource/lockfree_static_arena_resource.h>

#include <pthread.h>

TEST_GROUP(lockfree_static_arena_resource_CorrectnessTests)
{
    void setup() {}
    void teardown() {}
};

TEST(lockfree_static_arena_resource_CorrectnessTests, TestBasicAllocationAndAlignment)
{
    alignas(64) uint8_t buffer[4096];
    minstd::pmr::lockfree_static_arena_resource resource(buffer, sizeof(buffer));

    void* p1 = resource.allocate(32, 8);
    CHECK(p1 != nullptr);
    CHECK_EQUAL(0, reinterpret_cast<uintptr_t>(p1) % 64);

    void* p2 = resource.allocate(16, 16);
    CHECK(p2 != nullptr);
    CHECK_EQUAL(0, reinterpret_cast<uintptr_t>(p2) % 64);
    CHECK(p1 != p2);

    resource.deallocate(p1, 32, 8);
    resource.deallocate(p2, 16, 16);
    
    // Bump allocation continues
    void* p3 = resource.allocate(32, 8);
    CHECK(p3 != nullptr);
    CHECK(p3 != p1 && p3 != p2); // Still bumping
}

TEST(lockfree_static_arena_resource_CorrectnessTests, TestFallbackAllocation)
{
    alignas(64) uint8_t buffer[2 * (64 + 4)]; // Exact size for two 64-byte blocks and their free-list cells
    minstd::pmr::lockfree_static_arena_resource resource(buffer, sizeof(buffer));

    void* p1 = resource.allocate(32, 8); // Uses 64 bytes
    void* p2 = resource.allocate(32, 8); // Uses 64 bytes
    
    CHECK(p1 != nullptr);
    CHECK(p2 != nullptr);
    
    // Arena is exhausted now! Next allocation should fail since we hit the end
    // But it will search the free stack, which is empty. So it returns nullptr.
    void* p3 = resource.allocate(32, 8);
    CHECK(p3 == nullptr);

    // Free one block, putting it on the free stack
    resource.deallocate(p1, 32, 8);
    
    // Now allocation should hit the fallback logic and return the freed block
    void* p4 = resource.allocate(32, 8);
    CHECK(p4 == p1);
}

TEST(lockfree_static_arena_resource_CorrectnessTests, TestOversizedAlignmentRejection)
{
    alignas(64) uint8_t buffer[4096];
    minstd::pmr::lockfree_static_arena_resource resource(buffer, sizeof(buffer));

    // Request alignment > MIN_ALIGNMENT (64) -> Should reject by returning nullptr
    void* p = resource.allocate(128, 128);
    CHECK(p == nullptr);
}

TEST(lockfree_static_arena_resource_CorrectnessTests, RecyclesSmallAllocations)
{
    alignas(64) static char buffer[128];
    minstd::pmr::lockfree_static_arena_resource resource(buffer, sizeof(buffer));

    void *first = resource.allocate(8, 8);
    void *second = resource.allocate(8, 8);
    (void)second;

    resource.deallocate(first, 8, 8);

    CHECK_EQUAL(first, resource.allocate(8, 8)); //  before: nullptr
}

TEST(lockfree_static_arena_resource_CorrectnessTests, ConcurrentRecycleNeverReportsFalseOutOfMemory)
{
    //  Exactly one block per thread: a thread holds at most one, so supply always covers demand and any
    //      nullptr is a false out-of-memory.

    constexpr size_t NUM_THREADS = 8;
    constexpr size_t BLOCK = 256;
    constexpr size_t ITERATIONS = 20000;

    alignas(64) static uint8_t arena[NUM_THREADS * BLOCK / 64 * (64 + 4)]; //  the blocks and their free-list cells
    minstd::pmr::lockfree_static_arena_resource resource(arena, sizeof(arena));

    struct args_type
    {
        minstd::pmr::memory_resource *resource;
        minstd::atomic<bool> *go;
        minstd::atomic<uint32_t> *failures;
    };

    auto worker = [](void *arg) -> void *
    {
        auto *a = static_cast<args_type *>(arg);

        while (!a->go->load(minstd::memory_order_acquire))
        {
        }

        for (size_t i = 0; i < ITERATIONS; ++i)
        {
            void *block = a->resource->allocate(BLOCK, 64);

            if (block == nullptr)
            {
                a->failures->fetch_add(1);
                continue;
            }

            a->resource->deallocate(block, BLOCK, 64);
        }

        return nullptr;
    };

    minstd::atomic<bool> go{false};
    minstd::atomic<uint32_t> failures{0};
    args_type args[NUM_THREADS];
    pthread_t threads[NUM_THREADS];

    for (size_t t = 0; t < NUM_THREADS; ++t)
    {
        args[t] = {&resource, &go, &failures};
        CHECK_EQUAL(0, pthread_create(&threads[t], nullptr, worker, &args[t]));
    }

    go.store(true, minstd::memory_order_release);

    for (size_t t = 0; t < NUM_THREADS; ++t)
    {
        pthread_join(threads[t], nullptr);
    }

    CHECK_EQUAL(0u, failures.load()); //  before: > 0
}

//  arena_tagged_ptr: the static arena's free-bin head codec (an arena-relative block index plus a wide ABA counter).

namespace
{
    struct codec_node
    {
        size_t size;
        codec_node *next;
    };

    using codec_type = minstd::lockfree::arena_tagged_ptr<codec_node, 64>;
}

TEST(lockfree_static_arena_resource_CorrectnessTests, ArenaTaggedPtrCounterWidthFollowsArenaSize)
{
    //  The codec never dereferences, so fake arena addresses are fine.
    const void *base = reinterpret_cast<const void *>(uintptr_t{0x40000000});

    //  1 KiB: 16 units + 1 -> 5 index bits.  1 MiB: 16384 + 1 -> 15.  1 GiB: 2^24 + 1 -> 25.
    CHECK_EQUAL(5u, codec_type(base, 1024).index_bits());
    CHECK_EQUAL(59u, codec_type(base, 1024).counter_bits());
    CHECK_EQUAL(15u, codec_type(base, 1024 * 1024).index_bits());
    CHECK_EQUAL(25u, codec_type(base, size_t{1} << 30).index_bits());
    CHECK_EQUAL(39u, codec_type(base, size_t{1} << 30).counter_bits());
}

TEST(lockfree_static_arena_resource_CorrectnessTests, ArenaTaggedPtrRoundTripsFirstLastAndNull)
{
    const uintptr_t start = 0x40000010; //  not 64-aligned: the base rounds down to 0x40000000
    const size_t size = 4096 - 0x10;
    codec_type codec(reinterpret_cast<const void *>(start), size);

    codec_node *first = reinterpret_cast<codec_node *>(uintptr_t{0x40000040});
    codec_node *last = reinterpret_cast<codec_node *>(uintptr_t{0x40000000 + 4096 - 64});

    const uint64_t counters[] = {0, 1, 12345, codec.counter_mask()};

    for (uint64_t counter : counters)
    {
        POINTERS_EQUAL(first, codec.unpack_ptr(codec.pack(first, counter)));
        POINTERS_EQUAL(last, codec.unpack_ptr(codec.pack(last, counter)));
        POINTERS_EQUAL(nullptr, codec.unpack_ptr(codec.pack(nullptr, counter)));
        CHECK_EQUAL(counter, codec.unpack_counter(codec.pack(first, counter)));
        CHECK_EQUAL(counter, codec.unpack_counter(codec.pack(nullptr, counter)));
    }

    CHECK(codec.pack(first, 0) != codec.pack(last, 0));
    CHECK(codec.pack(nullptr, 0) != codec.pack(first, 0));
}

TEST(lockfree_static_arena_resource_CorrectnessTests, ArenaTaggedPtrCounterWrapsWithinItsField)
{
    alignas(64) static uint8_t buffer[64 * 8];
    codec_type codec(buffer, sizeof(buffer));

    codec_node *node = reinterpret_cast<codec_node *>(buffer + 3 * 64);
    const uint64_t max_counter = codec.counter_mask();

    //  counter + 1 at the top of the field wraps to 0 and leaves the index alone.
    const uint64_t at_max = codec.pack(node, max_counter);
    const uint64_t wrapped = codec.pack(node, codec.unpack_counter(at_max) + 1);
    CHECK_EQUAL(0u, codec.unpack_counter(wrapped));
    POINTERS_EQUAL(node, codec.unpack_ptr(wrapped));

    //  The stack's operations advance the counter across the wrap.
    using stack_type = minstd::lockfree::intrusive_tagged_stack<codec_node, minstd::lockfree::tagged_ptr<codec_node>, codec_node *, &codec_node::next>;

    struct adapter_type
    {
        codec_node *to_link(codec_node *p) const { return p; }
        codec_node *to_node(codec_node *p) const { return p; }
    } adapter;

    auto backoff = [](size_t &retries) { ++retries; };

    minstd::atomic<uint64_t> head{codec.pack(nullptr, max_counter - 1)};

    codec_node *a = reinterpret_cast<codec_node *>(buffer);
    codec_node *b = reinterpret_cast<codec_node *>(buffer + 64);

    stack_type::push(head, *a, adapter, backoff, codec); //  counter max
    stack_type::push(head, *b, adapter, backoff, codec); //  counter 0
    CHECK_EQUAL(0u, codec.unpack_counter(head.load()));
    POINTERS_EQUAL(b, codec.unpack_ptr(head.load()));

    POINTERS_EQUAL(b, stack_type::pop(head, adapter, backoff, codec));
    POINTERS_EQUAL(a, stack_type::pop(head, adapter, backoff, codec));
    POINTERS_EQUAL(nullptr, stack_type::pop(head, adapter, backoff, codec));

    stack_type::push(head, *a, adapter, backoff, codec);
    POINTERS_EQUAL(a, stack_type::steal_all(head, codec));
    POINTERS_EQUAL(nullptr, codec.unpack_ptr(head.load()));
    CHECK_EQUAL(4u, codec.unpack_counter(head.load())); //  2 pushes, 2 pops, a push and the steal after 0
}
