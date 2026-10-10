// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <lru_cache>

#include <__memory_resource/memory_heap_resource_adapter.h>
#include <__memory_resource/polymorphic_allocator.h>
#include <__memory_resource/tracking_memory_resource.h>
#include <single_block_memory_heap>

#include <../shared/poisoning_memory_resource.h>
#include "../shared/process_isolation.h"

#include <memory>

#define TEST_BUFFER_SIZE 65536

namespace
{
    static char buffer[TEST_BUFFER_SIZE];

    minstd::single_block_memory_heap test_heap(buffer, TEST_BUFFER_SIZE);
    minstd::pmr::memory_heap_resource_adapter test_heap_resource(test_heap);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (LRUCacheTests)
    {
        void setup()
        {
            CHECK_EQUAL(0, test_heap.bytes_in_use());
        }

        void teardown()
        {
            CHECK_EQUAL(0, test_heap.bytes_in_use());
        }
    };
#pragma GCC diagnostic pop

    class test_element
    {
    public:
        explicit test_element(uint32_t value)
            : value_(value)
        {
        }

        test_element(const test_element &) = default;

        uint32_t value() const
        {
            return value_;
        }

        bool operator<(const test_element &other) const
        {
            return value_ < other.value_;
        }

    private:
        uint32_t value_ = 0;
        char empty_space_[18];
    };

    using test_element_allocator = minstd::pmr::polymorphic_allocator<test_element>;

    using lru_cache_with_elements = minstd::lru_cache<uint32_t, test_element>;
    using lru_cache_with_elementsEntryHeapAllocator = minstd::pmr::polymorphic_allocator<lru_cache_with_elements::list_entry_type>;
    using lru_cache_with_elementsMapHeapAllocator = minstd::pmr::polymorphic_allocator<lru_cache_with_elements::map_entry_type>;

    using lru_cache_with_pointers = minstd::lru_cache<uint32_t, minstd::unique_ptr<test_element>>;
    using lru_cache_with_pointersEntryHeapAllocator = minstd::pmr::polymorphic_allocator<lru_cache_with_pointers::list_entry_type>;
    using lru_cache_with_pointersMapHeapAllocator = minstd::pmr::polymorphic_allocator<lru_cache_with_pointers::map_entry_type>;

    lru_cache_with_elementsEntryHeapAllocator cache_entry_allocator(&test_heap_resource);
    lru_cache_with_elementsMapHeapAllocator map_entry_allocator(&test_heap_resource);
    lru_cache_with_pointersEntryHeapAllocator cache_pointer_entry_allocator(&test_heap_resource);
    lru_cache_with_pointersMapHeapAllocator map_pointer_entry_allocator(&test_heap_resource);

    TEST(LRUCacheTests, BasicTests)
    {
        lru_cache_with_elements test_cache(12, cache_entry_allocator, map_entry_allocator);

        CHECK(test_cache.max_size() == 12);
        CHECK(test_cache.empty());
        CHECK_EQUAL(0, test_cache.size());

        for (size_t i = 1; i <= 6; ++i)
        {
            CHECK(test_cache.add(i, test_element(i)));
        }

        CHECK(!test_cache.empty());
        CHECK_EQUAL(6, test_cache.size());

        for (size_t i = 1; i <= 6; ++i)
        {
            auto entry = test_cache.find(i);

            CHECK(entry.has_value());
            CHECK_EQUAL(i, entry->get().value());
        }

        for (size_t i = 7; i <= 14; ++i)
        {
            const test_element const_element(i);
            CHECK(test_cache.add(i, const_element));
        }

        CHECK(!test_cache.add(14, test_element(14)));

        {
            //  Braces to control the scope of the const_element

            const test_element const_element(14);
            CHECK(!test_cache.add(14, const_element));
        }

        CHECK(!test_cache.find(1).has_value());
        CHECK(!test_cache.find(2).has_value());
        CHECK(test_cache.find(3).has_value());

        auto const_itr = const_cast<const lru_cache_with_elements &>(test_cache).begin();

        CHECK_EQUAL(3, (const_itr++)->value().value());
        CHECK_EQUAL(14, (const_itr++)->value().value());
        CHECK_EQUAL(13, (const_itr++)->value().value());
        CHECK_EQUAL(12, (const_itr++)->value().value());
        CHECK_EQUAL(11, (const_itr++)->value().value());
        CHECK_EQUAL(10, (const_itr++)->value().value());
        CHECK_EQUAL(9, (const_itr++)->value().value());
        CHECK_EQUAL(8, (const_itr++)->value().value());
        CHECK_EQUAL(7, (const_itr++)->value().value());
        CHECK_EQUAL(6, (const_itr++)->value().value());
        CHECK_EQUAL(5, (const_itr++)->value().value());
        CHECK_EQUAL(4, (const_itr++)->value().value());
        CHECK(const_itr == const_cast<const lru_cache_with_elements &>(test_cache).end());

        CHECK(test_cache.find(10).has_value());

        auto itr = test_cache.begin();

        CHECK_EQUAL(10, (itr++)->value().value());
        CHECK_EQUAL(3, (itr++)->value().value());
        CHECK_EQUAL(14, (itr++)->value().value());
        CHECK_EQUAL(13, (itr++)->value().value());
        CHECK_EQUAL(12, (itr++)->value().value());
        CHECK_EQUAL(11, (itr++)->value().value());
        CHECK_EQUAL(9, (itr++)->value().value());
        CHECK_EQUAL(8, (itr++)->value().value());
        CHECK_EQUAL(7, (itr++)->value().value());
        CHECK_EQUAL(6, (itr++)->value().value());
        CHECK_EQUAL(5, (itr++)->value().value());
        CHECK_EQUAL(4, (itr++)->value().value());
        CHECK(itr == test_cache.end());

        CHECK(test_cache.remove(10));
        CHECK(!test_cache.remove(10));

        itr = test_cache.begin();

        CHECK_EQUAL(3, (itr++)->value().value());
        CHECK_EQUAL(14, (itr++)->value().value());
        CHECK_EQUAL(13, (itr++)->value().value());
        CHECK_EQUAL(12, (itr++)->value().value());
        CHECK_EQUAL(11, (itr++)->value().value());
        CHECK_EQUAL(9, (itr++)->value().value());
        CHECK_EQUAL(8, (itr++)->value().value());
        CHECK_EQUAL(7, (itr++)->value().value());
        CHECK_EQUAL(6, (itr++)->value().value());
        CHECK_EQUAL(5, (itr++)->value().value());
        CHECK_EQUAL(4, (itr++)->value().value());
        CHECK(itr == test_cache.end());

        test_cache.remove(4);

        itr = test_cache.begin();

        CHECK_EQUAL(3, (itr++)->value().value());
        CHECK_EQUAL(14, (itr++)->value().value());
        CHECK_EQUAL(13, (itr++)->value().value());
        CHECK_EQUAL(12, (itr++)->value().value());
        CHECK_EQUAL(11, (itr++)->value().value());
        CHECK_EQUAL(9, (itr++)->value().value());
        CHECK_EQUAL(8, (itr++)->value().value());
        CHECK_EQUAL(7, (itr++)->value().value());
        CHECK_EQUAL(6, (itr++)->value().value());
        CHECK_EQUAL(5, (itr++)->value().value());
        CHECK(itr == test_cache.end());

        test_cache.remove(8);

        itr = test_cache.begin();

        CHECK_EQUAL(3, (itr++)->value().value());
        CHECK_EQUAL(14, (itr++)->value().value());
        CHECK_EQUAL(13, (itr++)->value().value());
        CHECK_EQUAL(12, (itr++)->value().value());
        CHECK_EQUAL(11, (itr++)->value().value());
        CHECK_EQUAL(9, (itr++)->value().value());
        CHECK_EQUAL(7, (itr++)->value().value());
        CHECK_EQUAL(6, (itr++)->value().value());
        CHECK_EQUAL(5, (itr++)->value().value());
        CHECK(itr == test_cache.end());

        test_cache.clear();

        CHECK(test_cache.empty());
        CHECK_EQUAL(0, test_cache.size());

        //  The destructor should clean up the heap allocated elements

        for (size_t i = 1; i <= 6; ++i)
        {
            CHECK(test_cache.add(i, test_element(i)));
        }

        CHECK(!test_cache.empty());
        CHECK_EQUAL(6, test_cache.size());
    }

    TEST(LRUCacheTests, BasicPointerElementTests)
    {
        lru_cache_with_pointers test_cache(12, cache_pointer_entry_allocator, map_pointer_entry_allocator);

        CHECK(test_cache.empty());
        CHECK_EQUAL(0, test_cache.size());

        for (size_t i = 1; i <= 6; ++i)
        {
            CHECK(test_cache.add(i, minstd::move(minstd::unique_ptr<test_element>(new (test_heap_resource.allocate(sizeof(test_element), alignof(test_element))) test_element(i), test_heap_resource))));
        }

        CHECK(!test_cache.empty());
        CHECK_EQUAL(6, test_cache.size());

        for (size_t i = 1; i <= 6; ++i)
        {
            auto entry = test_cache.find(i);

            CHECK(entry.has_value());
            CHECK_EQUAL(i, entry->get()->value());
        }

        for (size_t i = 7; i <= 14; ++i)
        {
            CHECK(test_cache.add(i, minstd::move(minstd::unique_ptr<test_element>(new (test_heap_resource.allocate(sizeof(test_element), alignof(test_element))) test_element(i), test_heap_resource))));
        }

        CHECK(!test_cache.add(14, minstd::move(minstd::unique_ptr<test_element>(new (test_heap_resource.allocate(sizeof(test_element), alignof(test_element))) test_element(14), test_heap_resource))));

        CHECK(!test_cache.find(1).has_value());
        CHECK(!test_cache.find(2).has_value());
        CHECK(test_cache.find(3).has_value());

        auto itr = test_cache.begin();

        CHECK_EQUAL(3, (itr++)->value()->value());
        CHECK_EQUAL(14, (itr++)->value()->value());
        CHECK_EQUAL(13, (itr++)->value()->value());
        CHECK_EQUAL(12, (itr++)->value()->value());
        CHECK_EQUAL(11, (itr++)->value()->value());
        CHECK_EQUAL(10, (itr++)->value()->value());
        CHECK_EQUAL(9, (itr++)->value()->value());
        CHECK_EQUAL(8, (itr++)->value()->value());
        CHECK_EQUAL(7, (itr++)->value()->value());
        CHECK_EQUAL(6, (itr++)->value()->value());
        CHECK_EQUAL(5, (itr++)->value()->value());
        CHECK_EQUAL(4, (itr++)->value()->value());
        CHECK(itr == test_cache.end());

        CHECK(test_cache.find(10).has_value());

        itr = test_cache.begin();

        CHECK_EQUAL(10, (itr++)->value()->value());
        CHECK_EQUAL(3, (itr++)->value()->value());
        CHECK_EQUAL(14, (itr++)->value()->value());
        CHECK_EQUAL(13, (itr++)->value()->value());
        CHECK_EQUAL(12, (itr++)->value()->value());
        CHECK_EQUAL(11, (itr++)->value()->value());
        CHECK_EQUAL(9, (itr++)->value()->value());
        CHECK_EQUAL(8, (itr++)->value()->value());
        CHECK_EQUAL(7, (itr++)->value()->value());
        CHECK_EQUAL(6, (itr++)->value()->value());
        CHECK_EQUAL(5, (itr++)->value()->value());
        CHECK_EQUAL(4, (itr++)->value()->value());
        CHECK(itr == test_cache.end());

        CHECK(test_cache.remove(10));
        CHECK(!test_cache.remove(10));

        itr = test_cache.begin();

        CHECK_EQUAL(3, (itr++)->value()->value());
        CHECK_EQUAL(14, (itr++)->value()->value());
        CHECK_EQUAL(13, (itr++)->value()->value());
        CHECK_EQUAL(12, (itr++)->value()->value());
        CHECK_EQUAL(11, (itr++)->value()->value());
        CHECK_EQUAL(9, (itr++)->value()->value());
        CHECK_EQUAL(8, (itr++)->value()->value());
        CHECK_EQUAL(7, (itr++)->value()->value());
        CHECK_EQUAL(6, (itr++)->value()->value());
        CHECK_EQUAL(5, (itr++)->value()->value());
        CHECK_EQUAL(4, (itr++)->value()->value());
        CHECK(itr == test_cache.end());

        CHECK(test_cache.remove(4));

        itr = test_cache.begin();

        CHECK_EQUAL(3, (itr++)->value()->value());
        CHECK_EQUAL(14, (itr++)->value()->value());
        CHECK_EQUAL(13, (itr++)->value()->value());
        CHECK_EQUAL(12, (itr++)->value()->value());
        CHECK_EQUAL(11, (itr++)->value()->value());
        CHECK_EQUAL(9, (itr++)->value()->value());
        CHECK_EQUAL(8, (itr++)->value()->value());
        CHECK_EQUAL(7, (itr++)->value()->value());
        CHECK_EQUAL(6, (itr++)->value()->value());
        CHECK_EQUAL(5, (itr++)->value()->value());
        CHECK(itr == test_cache.end());

        test_cache.remove(8);

        itr = test_cache.begin();

        CHECK_EQUAL(3, (itr++)->value()->value());
        CHECK_EQUAL(14, (itr++)->value()->value());
        CHECK_EQUAL(13, (itr++)->value()->value());
        CHECK_EQUAL(12, (itr++)->value()->value());
        CHECK_EQUAL(11, (itr++)->value()->value());
        CHECK_EQUAL(9, (itr++)->value()->value());
        CHECK_EQUAL(7, (itr++)->value()->value());
        CHECK_EQUAL(6, (itr++)->value()->value());
        CHECK_EQUAL(5, (itr++)->value()->value());
        CHECK(itr == test_cache.end());

        test_cache.clear();

        CHECK(test_cache.empty());
        CHECK_EQUAL(0, test_cache.size());

        //  The destructor should clean up the heap allocated elements

        for (size_t i = 1; i <= 6; ++i)
        {
            CHECK(test_cache.add(i, minstd::move(minstd::unique_ptr<test_element>(new (test_heap_resource.allocate(sizeof(test_element), alignof(test_element))) test_element(i), test_heap_resource))));
        }

        CHECK(!test_cache.empty());
        CHECK_EQUAL(6, test_cache.size());
    }

    TEST(LRUCacheTests, EvictionRemovesEvictedKeyFromMap)
    {
        minstd::pmr::test::poisoning_memory_resource entry_resource;
        minstd::pmr::test::poisoning_memory_resource map_resource;

        lru_cache_with_elementsEntryHeapAllocator entry_allocator(&entry_resource);
        lru_cache_with_elementsMapHeapAllocator map_allocator(&map_resource);

        {
            lru_cache_with_elements test_cache(2, entry_allocator, map_allocator);

            CHECK(test_cache.add(1, test_element(10)));
            CHECK(test_cache.add(2, test_element(20)));
            CHECK(test_cache.add(3, test_element(30))); //  evicts key 1

            CHECK_EQUAL(2, test_cache.size());

            //  If eviction read a freed key, key 1 is still in the map and this add is refused

            CHECK(test_cache.add(1, test_element(11)));
            CHECK(test_cache.find(1).has_value());
        }

        CHECK(entry_resource.freed_memory_intact());
        CHECK(map_resource.freed_memory_intact());
        CHECK_EQUAL(0, entry_resource.bytes_in_use());
        CHECK_EQUAL(0, map_resource.bytes_in_use());
    }

    TEST(LRUCacheTests, CacheHitDoesNotTouchTheMapAllocator)
    {
        //  find() erased and re-inserted the map entry on every hit, although move_front() relinks the same list
        //      node and the stored iterator stays valid.
        using cache_type = minstd::lru_cache<uint32_t, uint32_t>;

        minstd::pmr::tracking_memory_resource map_tracking(&test_heap_resource);
        minstd::pmr::polymorphic_allocator<cache_type::list_entry_type> entry_allocator(&test_heap_resource);
        minstd::pmr::polymorphic_allocator<cache_type::map_entry_type> map_allocator(&map_tracking);

        {
            cache_type cache(8, entry_allocator, map_allocator);

            for (uint32_t key = 1; key <= 4; ++key)
            {
                cache.add(key, key * 10);
            }

            const size_t allocations_before = map_tracking.allocation_count();

            for (uint32_t i = 0; i < 100; ++i)
            {
                const uint32_t key = 1 + (i % 4);
                auto found = cache.find(key);
                CHECK_TRUE(found.has_value());
                CHECK_EQUAL(key * 10, found.value().get());
            }

            CHECK_EQUAL(allocations_before, map_tracking.allocation_count()); //  before: + 100

            cache.find(2);
            CHECK_EQUAL(2u, cache.begin()->key()); //  most recently used is at the front

            CHECK_TRUE(cache.remove(2)); //  the map still finds the moved entry
            CHECK_FALSE(cache.find(2).has_value());
            CHECK_TRUE(cache.find(3).has_value());
        }
    }

    TEST(LRUCacheTests, ZeroCapacityCacheRejectsAdds)
    {
        //  With max_size 0, add() made room by evicting back() of the empty list.
        CHECK(minstd::pmr::test::runs_to_completion([]
                                                    {
                                                        using cache_type = minstd::lru_cache<uint32_t, uint32_t>;
                                                        minstd::pmr::polymorphic_allocator<cache_type::list_entry_type> entries(&test_heap_resource);
                                                        minstd::pmr::polymorphic_allocator<cache_type::map_entry_type> map(&test_heap_resource);
                                                        cache_type cache(0, entries, map);
                                                        if (cache.add(1, 1)) _exit(1);
                                                        uint32_t value = 2;
                                                        if (cache.add(2, minstd::move(value))) _exit(2);
                                                        if (cache.find(1).has_value()) _exit(3); })); //  before: child crashes in insure_space_exists()
    }
}
