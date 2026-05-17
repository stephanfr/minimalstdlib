// Copyright 2023 steve. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <stdint.h>

#include <minstdconfig.h>

#include <optional>

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(OptionalTests)
    {};
#pragma GCC diagnostic pop

    class test_element
    {
    public:
        test_element()
            : value1_(0),
              value2_(0)
        {
        }

        explicit test_element(uint32_t value1,
                             uint32_t value2)
            : value1_(value1),
              value2_(value2)
        {
        }

        test_element(const test_element &) = default;

        uint32_t value1() const
        {
            return value1_;
        }

        uint32_t &value1()
        {
            return value1_;
        }

        uint32_t value2() const
        {
            return value2_;
        }

        uint32_t &value2()
        {
            return value2_;
        }

    private:
        uint32_t value1_;
        uint32_t value2_;
    };

    class lifecycle_probe
    {
    public:
        lifecycle_probe() = default;

        explicit lifecycle_probe(uint32_t value)
            : value_(value)
        {
            live_instances_++;
            constructed_++;
        }

        lifecycle_probe(const lifecycle_probe &other)
            : value_(other.value_)
        {
            live_instances_++;
            constructed_++;
        }

        lifecycle_probe &operator=(const lifecycle_probe &other) = default;

        ~lifecycle_probe()
        {
            destroyed_++;
            live_instances_--;
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
        uint32_t value_ = 0;

        inline static int live_instances_ = 0;
        inline static int constructed_ = 0;
        inline static int destroyed_ = 0;
    };

    TEST(OptionalTests, PrimitivesTest)
    {
        minstd::optional<uint32_t> test_uint32_t1;

        CHECK(test_uint32_t1.has_value() == false);

        minstd::optional<uint32_t> test_uint32_t2(7);

        CHECK(test_uint32_t2.has_value() == true);
        CHECK(test_uint32_t2.value() == 7);
    }

    TEST(OptionalTests, ConstructorTests)
    {
        //  Constructor tests

        minstd::optional<test_element> test_elem1;

        CHECK(test_elem1.has_value() == false);

        minstd::optional<test_element> test_elem2( test_element(9,10) );

        CHECK(test_elem2.has_value() == true);
        CHECK(test_elem2.value().value1() == 9);
        CHECK(test_elem2.value().value2() == 10);
        CHECK((*test_elem2).value1() == 9);
        CHECK((*test_elem2).value2() == 10);

        minstd::optional<test_element> test_elem3( test_element(11, 12) );

        CHECK(test_elem3.has_value() == true);
        CHECK(test_elem3.value().value1() == 11);
        CHECK(test_elem3.value().value2() == 12);
        CHECK(test_elem3->value1() == 11);
        CHECK(test_elem3->value2() == 12);

        test_element te4(13, 14);

        minstd::optional<test_element> test_elem4( te4 );

        CHECK(test_elem4.has_value() == true);
        CHECK(test_elem4.value().value1() == 13);
        CHECK(test_elem4.value().value2() == 14);
        CHECK(test_elem4->value1() == 13);
        CHECK(test_elem4->value2() == 14);

        const test_element te5(15, 16);

        minstd::optional<test_element> test_elem5( te5 );

        CHECK(test_elem5.has_value() == true);
        CHECK(test_elem5.value().value1() == 15);
        CHECK(test_elem5.value().value2() == 16);
        CHECK(test_elem5->value1() == 15);
        CHECK(test_elem5->value2() == 16);

        //  Copy constructor test

        minstd::optional<test_element> test_elem1_copy( test_elem1 );

        CHECK(test_elem1_copy.has_value() == false);

        minstd::optional<test_element> test_elem2_copy( test_elem2 );

        CHECK(test_elem2_copy.has_value() == true);
        CHECK(test_elem2_copy.value().value1() == 9);
        CHECK(test_elem2_copy.value().value2() == 10);
        CHECK((*test_elem2_copy).value1() == 9);
        CHECK((*test_elem2_copy).value2() == 10);

        //  Const copy constructor test

        minstd::optional<test_element> test_elem1_copy2( (const minstd::optional<test_element>&)test_elem1 );

        CHECK(test_elem1_copy2.has_value() == false);

        minstd::optional<test_element> test_elem2_copy2( (const minstd::optional<test_element>&)test_elem2 );

        CHECK(test_elem2_copy2.has_value() == true);
        CHECK(test_elem2_copy2.value().value1() == 9);
        CHECK(test_elem2_copy2.value().value2() == 10);
        CHECK((*test_elem2_copy2).value1() == 9);
        CHECK((*test_elem2_copy2).value2() == 10);

        //  Move constructor const const operator test

        minstd::optional<test_element> test_elem1_copy3( minstd::move(test_elem1) );

        CHECK(test_elem1_copy3.has_value() == false);

        const minstd::optional<test_element> test_elem2_copy3( minstd::move(test_elem2) );

        CHECK(test_elem2_copy3.has_value() == true);
        CHECK(test_elem2_copy3.value().value1() == 9);
        CHECK(test_elem2_copy3.value().value2() == 10);
        CHECK((*test_elem2_copy3).value1() == 9);
        CHECK(test_elem2_copy3->value2() == 10);
    }

    TEST(OptionalTests, AssignmentTests)
    {
        //  Assignment tests

        minstd::optional<test_element> test_elem1;

        CHECK(test_elem1.has_value() == false);

        test_elem1 = test_element(9,10);

        CHECK(test_elem1.has_value() == true);
        CHECK(test_elem1.value().value1() == 9);
        CHECK(test_elem1.value().value2() == 10);
        CHECK((*test_elem1).value1() == 9);
        CHECK((*test_elem1).value2() == 10);

        test_elem1.reset();

        CHECK(test_elem1.has_value() == false);

        const test_element   const_te(11,12);

        test_elem1 = const_te;

        CHECK(test_elem1.has_value() == true);
        CHECK(test_elem1.value().value1() == 11);
        CHECK(test_elem1.value().value2() == 12);
        CHECK((*test_elem1).value1() == 11);
        CHECK((*test_elem1).value2() == 12);

        minstd::optional<test_element> test_elem2;

        test_elem2 = test_elem1;

        CHECK(test_elem2.has_value() == true);
        CHECK(test_elem2.value().value1() == 11);
        CHECK(test_elem2.value().value2() == 12);
        CHECK((*test_elem2).value1() == 11);
        CHECK((*test_elem2).value2() == 12);

    }

    TEST(OptionalTests, ResetCallsDestructor)
    {
        lifecycle_probe::reset_counters();
        lifecycle_probe source(7);

        {
            minstd::optional<lifecycle_probe> opt(source);

            CHECK(opt.has_value());
            CHECK_EQUAL(2, lifecycle_probe::live_instances());

            opt.reset();

            CHECK(!opt.has_value());
            CHECK_EQUAL(1, lifecycle_probe::live_instances());
            CHECK_EQUAL(1, lifecycle_probe::destroyed());
        }
    }

    TEST(OptionalTests, AssignFromEmptyClearsDestination)
    {
        lifecycle_probe::reset_counters();
        lifecycle_probe source(1);

        minstd::optional<lifecycle_probe> src_empty;
        minstd::optional<lifecycle_probe> dst(source);

        CHECK(dst.has_value());
        CHECK_EQUAL(2, lifecycle_probe::live_instances());

        dst = src_empty;

        CHECK(!dst.has_value());
        CHECK_EQUAL(1, lifecycle_probe::live_instances());
        CHECK_EQUAL(1, lifecycle_probe::destroyed());
    }

    TEST(OptionalTests, MoveConstructorPreservesSourceEngagement)
    {
        minstd::optional<uint32_t> source(42);

        minstd::optional<uint32_t> moved(minstd::move(source));

        CHECK(moved.has_value());
        CHECK_EQUAL(42u, moved.value());

        // std::optional keeps the source engaged after move construction.
        CHECK(source.has_value());
    }

    TEST(OptionalTests, MoveAssignmentPreservesSourceEngagement)
    {
        minstd::optional<uint32_t> source(77);
        minstd::optional<uint32_t> destination;

        destination = minstd::move(source);

        CHECK(destination.has_value());
        CHECK_EQUAL(77u, destination.value());

        // std::optional keeps the source engaged after move assignment.
        CHECK(source.has_value());
    }

    TEST(OptionalTests, MoveAssignmentFromEmptyClearsDestination)
    {
        minstd::optional<uint32_t> source;
        minstd::optional<uint32_t> destination(13);

        destination = minstd::move(source);

        CHECK(!destination.has_value());
    }

    TEST(OptionalTests, NulloptConstructorTest)
    {
        minstd::optional<uint32_t> opt(minstd::nullopt);

        CHECK(!opt.has_value());
    }

    TEST(OptionalTests, NulloptAssignmentClearsValue)
    {
        lifecycle_probe::reset_counters();
        lifecycle_probe source(42);

        minstd::optional<lifecycle_probe> opt(source);
        CHECK(opt.has_value());
        CHECK_EQUAL(2, lifecycle_probe::live_instances());

        opt = minstd::nullopt;

        CHECK(!opt.has_value());
        CHECK_EQUAL(1, lifecycle_probe::live_instances());
        CHECK_EQUAL(1, lifecycle_probe::destroyed());
    }

    TEST(OptionalTests, CopyInitializationTest)
    {
        //  Validates that removing 'explicit' from copy/move ctors allows
        //  copy-initialization syntax.
        minstd::optional<uint32_t> source(55u);
        minstd::optional<uint32_t> dest = source;

        CHECK(dest.has_value());
        CHECK_EQUAL(55u, dest.value());

        minstd::optional<uint32_t> empty_src;
        minstd::optional<uint32_t> empty_dest = empty_src;

        CHECK(!empty_dest.has_value());
    }

    TEST(OptionalTests, EmplaceTest)
    {
        //  Emplace into empty optional
        minstd::optional<test_element> opt;

        CHECK(!opt.has_value());

        test_element &ref = opt.emplace(3u, 4u);

        CHECK(opt.has_value());
        CHECK_EQUAL(3u, ref.value1());
        CHECK_EQUAL(4u, ref.value2());
        CHECK_EQUAL(3u, opt.value().value1());
        CHECK_EQUAL(4u, opt.value().value2());

        //  Emplace over an existing value — old value's destructor must be called
        lifecycle_probe::reset_counters();
        lifecycle_probe src(7);

        minstd::optional<lifecycle_probe> lopt(src);

        CHECK(lopt.has_value());
        CHECK_EQUAL(2, lifecycle_probe::live_instances());

        lopt.emplace(99u);

        CHECK(lopt.has_value());
        CHECK_EQUAL(2, lifecycle_probe::live_instances());
        CHECK_EQUAL(1, lifecycle_probe::destroyed());
    }

    TEST(OptionalTests, ValueOrTest)
    {
        minstd::optional<uint32_t> empty_opt;
        minstd::optional<uint32_t> value_opt(42u);

        CHECK_EQUAL(99u, empty_opt.value_or(99u));
        CHECK_EQUAL(42u, value_opt.value_or(99u));
    }

    TEST(OptionalTests, ComparisonOperatorsTest)
    {
        minstd::optional<uint32_t> empty1;
        minstd::optional<uint32_t> empty2;
        minstd::optional<uint32_t> value1(10u);
        minstd::optional<uint32_t> value2(10u);
        minstd::optional<uint32_t> value3(20u);

        //  optional == optional
        CHECK(empty1 == empty2);
        CHECK(value1 == value2);
        CHECK(!(empty1 == value1));
        CHECK(!(value1 == value3));

        //  optional != optional
        CHECK(!(empty1 != empty2));
        CHECK(!(value1 != value2));
        CHECK(empty1 != value1);
        CHECK(value1 != value3);

        //  optional == T  and  T == optional
        CHECK(value1 == 10u);
        CHECK(10u == value1);
        CHECK(!(empty1 == 10u));
        CHECK(!(value1 == 20u));

        //  optional != T  and  T != optional
        CHECK(!(value1 != 10u));
        CHECK(value1 != 20u);
        CHECK(empty1 != 10u);
        CHECK(!(10u != value1));
        CHECK(20u != value1);

        //  optional == nullopt  and  nullopt == optional
        CHECK(empty1 == minstd::nullopt);
        CHECK(minstd::nullopt == empty1);
        CHECK(!(value1 == minstd::nullopt));
        CHECK(!(minstd::nullopt == value1));

        //  optional != nullopt  and  nullopt != optional
        CHECK(value1 != minstd::nullopt);
        CHECK(minstd::nullopt != value1);
        CHECK(!(empty1 != minstd::nullopt));
        CHECK(!(minstd::nullopt != empty1));
    }
}
