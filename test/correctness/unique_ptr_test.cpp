// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <memory>
#include <type_traits>

#include <minimalcstdlib.h>

#include <__memory_resource/memory_resource.h>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(UniquePtrTests)
    {
    };
#pragma GCC diagnostic pop

    class recording_resource : public minstd::pmr::memory_resource
    {
    public:
        void *last_allocated_ = nullptr;
        size_t last_allocated_bytes_ = 0;
        void *last_deallocated_ = nullptr;
        size_t last_deallocated_bytes_ = 0;
        size_t last_deallocated_alignment_ = 0;

    private:
        alignas(64) unsigned char arena_[4096];
        size_t used_ = 0;

        void *do_allocate(size_t bytes, size_t alignment) override
        {
            used_ = (used_ + alignment - 1) & ~(alignment - 1);
            void *ptr = arena_ + used_;
            used_ += bytes;
            last_allocated_ = ptr;
            last_allocated_bytes_ = bytes;
            return ptr;
        }

        void do_deallocate(void *ptr, size_t bytes, size_t alignment) override
        {
            last_deallocated_ = ptr;
            last_deallocated_bytes_ = bytes;
            last_deallocated_alignment_ = alignment;
        }

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override { return this == &other; }
    };

    struct conversion_other_base
    {
        int other_value_ = 2;
    };

    struct conversion_base //  deliberately no virtual destructor
    {
        int base_value_ = 1;
    };

    struct conversion_derived : conversion_other_base, conversion_base //  conversion_base is NOT at offset 0
    {
        static inline int destroyed_ = 0;
        char payload_[1200]; //  > composite THRESHOLD_BYTES, so size-routing would pick the wrong pool

        ~conversion_derived() { destroyed_++; }
    };

    class counting_resource : public minstd::pmr::memory_resource
    {
    public:
        int allocate_calls_ = 0;
        int deallocate_calls_ = 0;

    private:
        void *do_allocate(size_t bytes, size_t) override
        {
            allocate_calls_++;
            return malloc(bytes);
        }

        void do_deallocate(void *ptr, size_t, size_t) override
        {
            deallocate_calls_++;
            free(ptr);
        }

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override
        {
            return this == &other;
        }
    };

    class tracked_element
    {
    public:
        explicit tracked_element(int value)
            : value_(value)
        {
            live_instances_++;
            constructed_++;
        }

        tracked_element(const tracked_element &other)
            : value_(other.value_)
        {
            live_instances_++;
            constructed_++;
        }

        ~tracked_element()
        {
            destroyed_++;
            live_instances_--;
        }

        int value() const
        {
            return value_;
        }

        void set_value(int value)
        {
            value_ = value;
        }

        static void reset_counters()
        {
            live_instances_ = 0;
            constructed_ = 0;
            destroyed_ = 0;
        }

        static int live_instances()
        {
            return live_instances_;
        }

        static int destroyed()
        {
            return destroyed_;
        }

    private:
        int value_ = 0;

        inline static int live_instances_ = 0;
        inline static int constructed_ = 0;
        inline static int destroyed_ = 0;
    };

    static minstd::unique_ptr<tracked_element> make_tracked_unique(counting_resource &resource, int value)
    {
        void *storage = resource.allocate(sizeof(tracked_element), alignof(tracked_element));

        CHECK(storage != nullptr);

        tracked_element *element = new (storage) tracked_element(value);

        return minstd::unique_ptr<tracked_element>(element, resource);
    }

    TEST(UniquePtrTests, TypeTraitsAreMoveOnly)
    {
        using ptr_type = minstd::unique_ptr<tracked_element>;

        static_assert(!minstd::is_copy_constructible_v<ptr_type>);
        static_assert(!minstd::is_copy_assignable_v<ptr_type>);
        static_assert(minstd::is_move_constructible_v<ptr_type>);
        static_assert(minstd::is_move_assignable_v<ptr_type>);

        CHECK(true);
    }

    TEST(UniquePtrTests, BasicAccessAndLifetime)
    {
        counting_resource resource;
        tracked_element::reset_counters();

        {
            auto ptr = make_tracked_unique(resource, 42);

            CHECK(ptr);
            CHECK(ptr.get() != nullptr);
            CHECK_EQUAL(42, ptr->value());
            CHECK_EQUAL(42, (*ptr).value());

            ptr->set_value(7);
            CHECK_EQUAL(7, (*ptr).value());

            CHECK_EQUAL(1, tracked_element::live_instances());
        }

        CHECK_EQUAL(0, tracked_element::live_instances());
        CHECK_EQUAL(1, tracked_element::destroyed());
        CHECK_EQUAL(1, resource.allocate_calls_);
        CHECK_EQUAL(1, resource.deallocate_calls_);
    }

    TEST(UniquePtrTests, MoveConstructorTransfersOwnership)
    {
        counting_resource resource;
        tracked_element::reset_counters();

        auto source = make_tracked_unique(resource, 111);
        tracked_element *raw_ptr = source.get();

        auto destination = minstd::move(source);

        CHECK(!source);
        CHECK(source.get() == nullptr);
        CHECK(destination.get() == raw_ptr);
        CHECK_EQUAL(111, destination->value());

        destination = minstd::unique_ptr<tracked_element>();

        CHECK_EQUAL(0, tracked_element::live_instances());
        CHECK_EQUAL(1, tracked_element::destroyed());
        CHECK_EQUAL(1, resource.allocate_calls_);
        CHECK_EQUAL(1, resource.deallocate_calls_);
    }

    TEST(UniquePtrTests, MoveAssignmentReleasesExistingOwnership)
    {
        counting_resource resource;
        tracked_element::reset_counters();

        auto destination = make_tracked_unique(resource, 1);
        auto source = make_tracked_unique(resource, 2);

        tracked_element *source_raw = source.get();

        destination = minstd::move(source);

        CHECK(!source);
        CHECK(source.get() == nullptr);
        CHECK(destination.get() == source_raw);
        CHECK_EQUAL(2, destination->value());

        // Destination's original pointee must be destroyed/deallocated on assignment.
        CHECK_EQUAL(1, tracked_element::destroyed());
        CHECK_EQUAL(1, resource.deallocate_calls_);

        destination = minstd::unique_ptr<tracked_element>();

        CHECK_EQUAL(0, tracked_element::live_instances());
        CHECK_EQUAL(2, tracked_element::destroyed());
        CHECK_EQUAL(2, resource.allocate_calls_);
        CHECK_EQUAL(2, resource.deallocate_calls_);
    }

    TEST(UniquePtrTests, ReleaseDetachesOwnership)
    {
        counting_resource resource;
        tracked_element::reset_counters();

        tracked_element *raw = nullptr;

        {
            auto ptr = make_tracked_unique(resource, 99);
            raw = ptr.release();

            CHECK(!ptr);
            CHECK(ptr.get() == nullptr);
            CHECK(raw != nullptr);
            CHECK_EQUAL(99, raw->value());
        }

        // release() detaches ownership; unique_ptr should not destroy/deallocate.
        CHECK_EQUAL(1, tracked_element::live_instances());
        CHECK_EQUAL(0, tracked_element::destroyed());
        CHECK_EQUAL(1, resource.allocate_calls_);
        CHECK_EQUAL(0, resource.deallocate_calls_);

        raw->~tracked_element();
        resource.deallocate(raw, sizeof(tracked_element), alignof(tracked_element));

        CHECK_EQUAL(0, tracked_element::live_instances());
        CHECK_EQUAL(1, tracked_element::destroyed());
        CHECK_EQUAL(1, resource.deallocate_calls_);
    }

    TEST(UniquePtrTests, ConvertingMoveDestroysAndFreesTheOriginalAllocation)
    {
        recording_resource resource;
        conversion_derived::destroyed_ = 0;

        {
            minstd::unique_ptr<conversion_derived> derived(new (resource.allocate(sizeof(conversion_derived), alignof(conversion_derived))) conversion_derived(), resource);

            minstd::unique_ptr<conversion_base> base(minstd::move(derived));

            CHECK(derived.get() == nullptr);
            CHECK_EQUAL(1, base->base_value_);
        }

        CHECK_EQUAL(1, conversion_derived::destroyed_);                                  //  before: 0
        CHECK(resource.last_deallocated_ == resource.last_allocated_);                    //  before: Base subobject address
        CHECK_EQUAL(sizeof(conversion_derived), resource.last_deallocated_bytes_);        //  before: sizeof(conversion_base)
        CHECK_EQUAL(alignof(conversion_derived), resource.last_deallocated_alignment_);
    }

    TEST(UniquePtrTests, ConvertingMoveAssignmentFreesTheOriginalAllocation)
    {
        recording_resource resource;
        conversion_derived::destroyed_ = 0;

        {
            minstd::unique_ptr<conversion_base> base;
            base = minstd::unique_ptr<conversion_derived>(new (resource.allocate(sizeof(conversion_derived), alignof(conversion_derived))) conversion_derived(), resource);
        }

        CHECK_EQUAL(1, conversion_derived::destroyed_);
        CHECK(resource.last_deallocated_ == resource.last_allocated_);
        CHECK_EQUAL(sizeof(conversion_derived), resource.last_deallocated_bytes_);
    }

    TEST(UniquePtrTests, SelfMoveAssignmentKeepsObject) //  pass-1 #27, same function
    {
        recording_resource resource;
        minstd::unique_ptr<int> ptr(new (resource.allocate(sizeof(int), alignof(int))) int(42), resource);

        auto &alias = ptr;
        ptr = minstd::move(alias);

        CHECK(ptr.get() != nullptr); //  before: destroyed and null
        CHECK_EQUAL(42, *ptr);
    }
}