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
    alignas(64) uint8_t buffer[128]; // Exact size for two blocks (64 each)
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

    alignas(64) static uint8_t arena[NUM_THREADS * BLOCK];
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
