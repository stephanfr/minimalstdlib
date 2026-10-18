// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//
//  Regression tests for contract and lifetime fixes in the smaller containers and utilities.
//

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <array>
#include <lru_cache>
#include <single_block_memory_heap>
#include <vector>

#include <lockfree/intrusive_payload_handle>

#include <__memory_resource/memory_heap_resource_adapter.h>
#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include "../shared/process_isolation.h"

namespace
{
    TEST_GROUP (LibraryContractsTests)
    {
    };

    unsigned contract_violations = 0;

    void count_violation(const char *, const char *, const char *, int)
    {
        contract_violations++;
    }

    //  Counts the contract violations reported while it is alive

    class violation_counter
    {
    public:
        violation_counter()
            : previous_(minstd::set_failure_hook(count_violation))
        {
            contract_violations = 0;
        }

        ~violation_counter()
        {
            minstd::set_failure_hook(previous_);
        }

        unsigned count() const
        {
            return contract_violations;
        }

    private:
        minstd::failure_hook_type previous_;
    };

    //
    //  vector: a full vector reports instead of silently dropping the element
    //

    using small_vector = minstd::vector<uint32_t, 4>;
    using small_vector_allocator = minstd::pmr::polymorphic_allocator<uint32_t>;

    TEST(LibraryContractsTests, PushBackOnAFullVectorIsAViolation)
    {
        static char buffer[1024];
        minstd::pmr::monotonic_buffer_resource resource(buffer, sizeof(buffer), nullptr);
        small_vector_allocator allocator(&resource);

        small_vector vec(allocator);

        violation_counter violations;

        for (uint32_t i = 0; i < 4; i++)
        {
            vec.push_back(i);
        }

        CHECK_EQUAL(0u, violations.count());

        vec.push_back(99); //  before: dropped without a report

        CHECK_EQUAL(1u, violations.count());
        CHECK_EQUAL(4u, vec.size());
        CHECK_EQUAL(3u, vec.back());
    }

    TEST(LibraryContractsTests, InitializerListLongerThanTheCapacityIsAViolation)
    {
        static char buffer[1024];
        minstd::pmr::monotonic_buffer_resource resource(buffer, sizeof(buffer), nullptr);
        small_vector_allocator allocator(&resource);

        violation_counter violations;

        small_vector fits({1, 2, 3, 4}, allocator);
        CHECK_EQUAL(0u, violations.count());

        small_vector truncated({1, 2, 3, 4, 5, 6}, allocator); //  before: silently truncated
        CHECK_EQUAL(1u, violations.count());
        CHECK_EQUAL(4u, truncated.size());
    }

    //
    //  lru_cache: an entry the map cannot take is not reported as added
    //

    using test_cache = minstd::lru_cache<uint32_t, uint32_t>;

    TEST(LibraryContractsTests, LRUCacheAddFailsWhenTheMapCannotTakeTheEntry)
    {
        static char list_buffer[4096];
        minstd::single_block_memory_heap list_heap(list_buffer, sizeof(list_buffer));
        minstd::pmr::memory_heap_resource_adapter list_resource(list_heap);
        minstd::pmr::polymorphic_allocator<test_cache::list_entry_type> list_allocator(&list_resource);

        //  The map's resource has no memory at all

        static char map_buffer[1];
        minstd::pmr::monotonic_buffer_resource map_resource(map_buffer, sizeof(map_buffer), nullptr);
        minstd::pmr::polymorphic_allocator<test_cache::map_entry_type> map_allocator(&map_resource);

        {
            test_cache cache(4, list_allocator, map_allocator);

            CHECK_FALSE(cache.add(1, 10)); //  before: true, with the entry only in the list
            CHECK_FALSE(cache.find(1).has_value());

            uint32_t value = 20;
            CHECK_FALSE(cache.add(2, minstd::move(value)));
        }

        //  The list entries were given back

        CHECK_EQUAL(0, list_heap.bytes_in_use());
    }

    //
    //  memory_heap: freeing nullptr is a no-op
    //

    TEST(LibraryContractsTests, MemoryHeapDeallocateNullIsANoOp)
    {
        static char heap_buffer[4096];
        minstd::single_block_memory_heap heap(heap_buffer, sizeof(heap_buffer));

        violation_counter violations;

        uint32_t *block = heap.allocate_block<uint32_t>(4);
        CHECK(block != nullptr);

        heap.deallocate_block<uint32_t>(nullptr, 4); //  before: read a header just below address 0

        CHECK_EQUAL(0u, violations.count());

        heap.deallocate_block(block, 4);
        CHECK_EQUAL(0, heap.bytes_in_use());
    }

    //
    //  intrusive_payload_handle: assigning from a handle the old payload owns
    //

    struct linked_payload
    {
        static inline unsigned destroyed = 0;

        uint32_t refs = 0;
        uint32_t id;
        minstd::lockfree::intrusive_payload_handle<linked_payload> next;

        explicit linked_payload(uint32_t payload_id)
            : id(payload_id)
        {
        }

        void add_ref()
        {
            refs++;
        }

        void release_ref()
        {
            if (--refs == 0)
            {
                destroyed++;
                delete this;
            }
        }
    };

    TEST(LibraryContractsTests, PayloadHandleAssignFromInsideTheReleasedPayload)
    {
        using handle = minstd::lockfree::intrusive_payload_handle<linked_payload>;

        linked_payload::destroyed = 0;

        {
            handle head(new linked_payload(1));
            head->next = handle(new linked_payload(2));
            head->next->next = handle(new linked_payload(3));

            //  head is the only reference to payload 1, and the source lives inside it.  Before the fix the
            //      old payload was released (and freed) before the source was read.

            head = head->next;
            CHECK_EQUAL(2u, head->id);
            CHECK_EQUAL(1u, linked_payload::destroyed);

            head = minstd::move(head->next);
            CHECK_EQUAL(3u, head->id);
            CHECK_EQUAL(2u, linked_payload::destroyed);

            head = head; //  self-assignment keeps the payload
            CHECK_EQUAL(3u, head->id);
            CHECK_EQUAL(1u, head->refs);
        }

        CHECK_EQUAL(3u, linked_payload::destroyed);
    }

    //
    //  Assert macros are single statements
    //

    TEST(LibraryContractsTests, AssertMacrosAreSingleStatements)
    {
        violation_counter violations;

        bool take_branch = false;

        //  With a bare { } body, the ';' after the macro ended the if, so this 'else' did not compile

        if (take_branch)
            MINIMAL_STD_ASSERT(false);
        else
            MINIMAL_STD_ASSERT(true);

        if (!take_branch)
            MINIMAL_STD_FAIL("reported");
        else
            take_branch = true;

        CHECK_EQUAL(1u, violations.count());
    }

    //
    //  array<T, 0>: element access has nothing to return a reference to
    //

    TEST(LibraryContractsTests, ZeroLengthArrayIsUsable)
    {
        minstd::array<uint32_t, 0> empty;

        CHECK_EQUAL(0u, empty.size());
        CHECK(empty.begin() == empty.end());

        //  operator[] used to 'return nullptr' as a reference, which did not compile once instantiated.
        //      There is no element, so indexing traps.

        CHECK_FALSE(minstd::pmr::test::runs_to_completion([]
                                                          {
                                                              minstd::array<uint32_t, 0> nothing;
                                                              volatile uint32_t value = nothing[0];
                                                              (void)value; }));
    }
}
