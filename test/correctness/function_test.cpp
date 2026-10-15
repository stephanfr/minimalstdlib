// Copyright 2023 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <functional>

#include <fixed_string>

#include <new>

//
//  Adapted from cplusplus.com reference for std::function
//

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(FunctionTests)
    {};
#pragma GCC diagnostic pop

    // a function:
    int half(int x) { return x / 2; }

    // a function object class:
    struct third_t
    {
        int operator()(int x) { return x / 3; }
    };

    // a class with data members:
    struct my_value
    {
        int value;
        int fifth() { return value / 5; }
    };

    template <class _Ty = void>
    struct negate
    {
        constexpr _Ty operator()(const _Ty &_Left) const
        {
            return -_Left;
        }
    };

    TEST(FunctionTests, BasicTests)
    {
        minstd::function<int(int)> fn1 = half;      // function
        minstd::function<int(int)> fn2 = &half;     // function pointer
        minstd::function<int(int)> fn3 = third_t(); // function object
        minstd::function<int(int)> fn4 = [](int x) { return x / 4; };   // lambda expression
        minstd::function<int(int)> fn5 = negate<int>(); // standard function object

        CHECK_EQUAL( fn1(60), 30 );
        CHECK_EQUAL( fn2(60), 30 );
        CHECK_EQUAL( fn3(60), 20 );
        CHECK_EQUAL( fn4(60), 15 );
        CHECK_EQUAL( fn5(60), -60 );

        //  Class instance with members:
        minstd::function<int(my_value &)> value = [](my_value &value){ return value.value; }; // lambda to return data member
        minstd::function<int(my_value &)> fifth = [](my_value &value){ return value.fifth(); }; // lambda to call member function

        my_value sixty{60};

        CHECK_EQUAL( value(sixty), 60 );
        CHECK_EQUAL( fifth(sixty), 12 );
    }

    struct counted_functor
    {
        int id;

        explicit counted_functor(int functor_id)
            : id(functor_id)
        {
            live++;
        }

        counted_functor(const counted_functor &other)
            : id(other.id)
        {
            live++;
        }

        ~counted_functor()
        {
            live--;
        }

        int operator()() const
        {
            return id;
        }

        inline static int live = 0;
    };

    TEST(FunctionTests, AssignmentDestroysTheOldFunctorAndCopiesTheNewOne)
    {
        //  There was no assignment operator: the implicit one copied the stored functor bytewise, so the old functor
        //      was never destroyed and the new one was destroyed twice.
        counted_functor::live = 0;

        {
            minstd::function<int()> f(counted_functor(1));
            minstd::function<int()> g(counted_functor(2));
            CHECK_EQUAL(2, counted_functor::live);

            f = g;
            CHECK_EQUAL(2, f());
            CHECK_EQUAL(2, g());
            CHECK_EQUAL(2, counted_functor::live); //  f's old functor destroyed, a copy of g's constructed

            f = minstd::function<int()>(counted_functor(3));
            CHECK_EQUAL(3, f());
            CHECK_EQUAL(2, counted_functor::live);

            f = f;
            CHECK_EQUAL(3, f());
            CHECK_EQUAL(2, counted_functor::live);

            minstd::function<int()> empty;
            f = empty;
            CHECK_FALSE(static_cast<bool>(f));
            CHECK_EQUAL(1, counted_functor::live);
        }

        CHECK_EQUAL(0, counted_functor::live);
    }

    TEST(FunctionTests, ByValueSignatureAcceptsLvalues)
    {
        minstd::function<int(int)> twice = [](int x) { return 2 * x; };
        int argument = 21;
        CHECK_EQUAL(42, twice(argument)); //  operator()(Args&&...) rejected lvalues
    }

    TEST(FunctionTests, StorageIsAlignedForAnyFunctor)
    {
        struct alignas(16) aligned_capture
        {
            long double value;

            long double operator()() const
            {
                return value;
            }
        };

        minstd::function<long double()> f = aligned_capture{2.5L};
        CHECK(f() == 2.5L);
    }
}