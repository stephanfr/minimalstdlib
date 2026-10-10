// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>
#include <minstdconfig.h>
#include <__memory_resource/lockfree_bitblock_resource.h>
#include <__memory_resource/single_block_resource.h>
#include <stdint.h>

#include <../shared/process_isolation.h>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(LockfreeBitblockResourceTests)
    {
    };
#pragma GCC diagnostic pop

    constexpr size_t buffer_size = 4 * 1048576; // 4 MB
    alignas(64) char test_buffer[buffer_size];
    
    using bitblock_resource_type = minstd::pmr::lockfree_bitblock_resource<64, 1024, 32, 48, false>;

    //  Forwards to an upstream resource and records whether every allocation was 64-byte aligned
    class alignment_recording_resource : public minstd::pmr::memory_resource
    {
    public:
        explicit alignment_recording_resource(minstd::pmr::memory_resource *upstream) : upstream_(upstream) {}

        bool all_cache_line_aligned() const { return all_cache_line_aligned_; }
        size_t allocation_count() const { return allocation_count_; }

    private:
        void *do_allocate(size_t bytes, size_t alignment) override
        {
            void *ptr = upstream_->allocate(bytes, alignment);
            allocation_count_++;
            all_cache_line_aligned_ = all_cache_line_aligned_ && ((reinterpret_cast<uintptr_t>(ptr) % 64) == 0);
            return ptr;
        }

        void do_deallocate(void *ptr, size_t bytes, size_t alignment) override { upstream_->deallocate(ptr, bytes, alignment); }

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override { return this == &other; }

        minstd::pmr::memory_resource *upstream_;
        bool all_cache_line_aligned_ = true;
        size_t allocation_count_ = 0;
    };
}

TEST(LockfreeBitblockResourceTests, ContiguousScanIntegrity)
{
    minstd::pmr::single_block_resource upstream(test_buffer, buffer_size);
    bitblock_resource_type small_pool(&upstream, 2);

    constexpr size_t max_contiguous_payload = 1008;

    void* max_contiguous_ptr = small_pool.allocate(max_contiguous_payload, 16);
    CHECK(max_contiguous_ptr != nullptr);
    CHECK((uintptr_t)max_contiguous_ptr >= (uintptr_t)test_buffer && (uintptr_t)max_contiguous_ptr < (uintptr_t)test_buffer + buffer_size);

    // Fill the pool to test fragmentation
    void* pointers[1024];
    for (int i = 0; i < 1000; i++) {
        pointers[i] = small_pool.allocate(64, 16);
        CHECK(pointers[i] != nullptr);
    }

    // Free interleaving to cause fragmentation
    for (int i = 0; i < 1000; i += 2) {
        small_pool.deallocate(pointers[i], 64, 16);
    }

    // Try a large aligned alloc—should fail or find another fresh block rather than the fragmented one
    void* large_ptr = small_pool.allocate(max_contiguous_payload, 16);
    CHECK(large_ptr != nullptr);
    CHECK((uintptr_t)large_ptr >= (uintptr_t)test_buffer && (uintptr_t)large_ptr < (uintptr_t)test_buffer + buffer_size);

    // Release the max chunk to let it recombine seamlessly internally over scanning
    small_pool.deallocate(max_contiguous_ptr, max_contiguous_payload, 16);

    // After out of order free, we can still re-allocate
    void* new_max = small_pool.allocate(max_contiguous_payload, 16);
    CHECK(new_max != nullptr);
    CHECK((uintptr_t)new_max >= (uintptr_t)test_buffer && (uintptr_t)new_max < (uintptr_t)test_buffer + buffer_size);

    // Clean up
    small_pool.deallocate(new_max, max_contiguous_payload, 16);
    small_pool.deallocate(large_ptr, max_contiguous_payload, 16);
    
    for (int i = 1; i < 1000; i += 2) {
        small_pool.deallocate(pointers[i], 64, 16);
    }
}

TEST(LockfreeBitblockResourceTests, BlocksAreCacheLineAligned)
{
    minstd::pmr::single_block_resource upstream(test_buffer, buffer_size);
    alignment_recording_resource recorder(&upstream);

    {
        bitblock_resource_type small_pool(&recorder, 2);   //  constructor allocates one block per arena

        //  Exhaust the first blocks so add_new_block() runs too (1024 x 64-byte elements per block)
        void *pointers[2100];
        for (size_t i = 0; i < 2100; i++)
        {
            pointers[i] = small_pool.allocate(48, 16);
            CHECK(pointers[i] != nullptr);
        }

        for (size_t i = 0; i < 2100; i++)
        {
            small_pool.deallocate(pointers[i], 48, 16);
        }
    }

    CHECK(recorder.allocation_count() >= 3);
    CHECK(recorder.all_cache_line_aligned());   //  before: blocks are 16-aligned (48 mod 64)
}

TEST(LockfreeBitblockResourceTests, FailedUpstreamAllocationIsSurvivable)
{
    CHECK(minstd::pmr::test::runs_to_completion([]
    {
        class failing_resource : public minstd::pmr::memory_resource
        {
            void *do_allocate(size_t, size_t) override { return nullptr; }
            void do_deallocate(void *, size_t, size_t) override {}
            bool do_is_equal(const minstd::pmr::memory_resource &o) const noexcept override { return this == &o; }
        } upstream;

        minstd::pmr::lockfree_bitblock_resource<64, 1024, 4, 8> resource(&upstream, 2);
        if (resource.allocate(32, 16) != nullptr) _exit(1);
    })); //  before: child segfaults in the constructor
}
