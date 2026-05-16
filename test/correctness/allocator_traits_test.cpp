// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <allocator>
#include <allocator_traits>
#include <type_traits>

#include <stdint.h>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(AllocatorTraitsTests)
    {
    };
#pragma GCC diagnostic pop

    class tracking_element
    {
    public:
        explicit tracking_element(uint32_t value)
            : value_(value)
        {
            live_count_++;
            constructed_count_++;
        }

        tracking_element(const tracking_element &other)
            : value_(other.value_)
        {
            live_count_++;
            constructed_count_++;
        }

        ~tracking_element()
        {
            live_count_--;
            destroyed_count_++;
        }

        static void reset_counters()
        {
            live_count_ = 0;
            constructed_count_ = 0;
            destroyed_count_ = 0;
        }

        static int live_count()
        {
            return live_count_;
        }

        static int constructed_count()
        {
            return constructed_count_;
        }

        static int destroyed_count()
        {
            return destroyed_count_;
        }

        uint32_t value() const
        {
            return value_;
        }

    private:
        uint32_t value_ = 0;

        inline static int live_count_ = 0;
        inline static int constructed_count_ = 0;
        inline static int destroyed_count_ = 0;
    };

    template <typename T>
    class test_allocator_type : public minstd::allocator<T>
    {
    public:
        using value_type = T;

        size_t max_size() const noexcept override
        {
            return 16;
        }

        T *allocate(size_t num_elements) override
        {
            last_allocated_elements = num_elements;
            return reinterpret_cast<T *>(storage_);
        }

        void deallocate(T *ptr, size_t num_elements) override
        {
            last_deallocated_ptr = ptr;
            last_deallocated_elements = num_elements;
        }

        size_t last_allocated_elements = 0;
        size_t last_deallocated_elements = 0;
        T *last_deallocated_ptr = nullptr;

    private:
        alignas(T) unsigned char storage_[sizeof(T) * 16];
    };

    template <typename T>
    class custom_construct_allocator_type : public test_allocator_type<T>
    {
    public:
        template <typename... args_type>
        void construct(T *ptr, args_type &&...args)
        {
            construct_calls++;
            ::new ((void *)ptr) T(minstd::forward<args_type>(args)...);
        }

        void destroy(T *ptr)
        {
            destroy_calls++;
            ptr->~T();
        }

        size_t construct_calls = 0;
        size_t destroy_calls = 0;
    };

    TEST(AllocatorTraitsTests, TypeAliasesAndRebind)
    {
        using traits_type = minstd::allocator_traits<test_allocator_type<int>>;

        CHECK_TRUE((minstd::is_same<traits_type::allocator_type, test_allocator_type<int>>::value));
        CHECK_TRUE((minstd::is_same<traits_type::value_type, int>::value));
        CHECK_TRUE((minstd::is_same<traits_type::pointer, int *>::value));
        CHECK_TRUE((minstd::is_same<traits_type::const_pointer, const int *>::value));

        CHECK_TRUE((minstd::is_same<traits_type::rebind_alloc<uint32_t>, test_allocator_type<uint32_t>>::value));
    }

    TEST(AllocatorTraitsTests, AllocateDeallocateAndMaxSizeForward)
    {
        test_allocator_type<int> allocator;
        using traits_type = minstd::allocator_traits<test_allocator_type<int>>;

        int *block = traits_type::allocate(allocator, 4);
        CHECK(block != nullptr);
        CHECK_EQUAL(4u, allocator.last_allocated_elements);
        CHECK_EQUAL(16u, traits_type::max_size(allocator));

        traits_type::deallocate(allocator, block, 4);
        CHECK_EQUAL(4u, allocator.last_deallocated_elements);
        CHECK(block == allocator.last_deallocated_ptr);
    }

    TEST(AllocatorTraitsTests, FallbackConstructAndDestroy)
    {
        test_allocator_type<tracking_element> allocator;
        using traits_type = minstd::allocator_traits<test_allocator_type<tracking_element>>;

        tracking_element::reset_counters();

        tracking_element *block = traits_type::allocate(allocator, 1);
        traits_type::construct(allocator, block, 99u);

        CHECK_EQUAL(99u, block->value());
        CHECK_EQUAL(1, tracking_element::live_count());
        CHECK_EQUAL(1, tracking_element::constructed_count());

        traits_type::destroy(allocator, block);

        CHECK_EQUAL(0, tracking_element::live_count());
        CHECK_EQUAL(1, tracking_element::destroyed_count());
    }

    TEST(AllocatorTraitsTests, UsesAllocatorConstructDestroyHooks)
    {
        custom_construct_allocator_type<tracking_element> allocator;
        using traits_type = minstd::allocator_traits<custom_construct_allocator_type<tracking_element>>;

        tracking_element::reset_counters();

        tracking_element *block = traits_type::allocate(allocator, 1);
        traits_type::construct(allocator, block, 321u);
        traits_type::destroy(allocator, block);

        CHECK_EQUAL(1u, allocator.construct_calls);
        CHECK_EQUAL(1u, allocator.destroy_calls);
        CHECK_EQUAL(1, tracking_element::constructed_count());
        CHECK_EQUAL(1, tracking_element::destroyed_count());
    }
}
