// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <stdint.h>

#include <minstdconfig.h>

#include <expected>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(ExpectedTests)
    {};
#pragma GCC diagnostic pop

    enum class parse_error : uint32_t
    {
        TOO_SHORT = 1,
        BAD_CHECKSUM,
    };

    //  Counts its live instances, so tests can see that each state is constructed and destroyed exactly once.

    class tracked
    {
    public:
        static int live;

        explicit tracked(uint32_t value)
            : value_(value)
        {
            live++;
        }

        tracked(const tracked &other)
            : value_(other.value_)
        {
            live++;
        }

        tracked(tracked &&other)
            : value_(other.value_)
        {
            other.value_ = 0;
            live++;
        }

        ~tracked()
        {
            live--;
        }

        tracked &operator=(const tracked &) = default;

        uint32_t value() const
        {
            return value_;
        }

    private:
        uint32_t value_;
    };

    int tracked::live = 0;

    minstd::expected<uint32_t, parse_error> parse_length(uint32_t length)
    {
        if (length < 20)
        {
            return minstd::unexpected(parse_error::TOO_SHORT);
        }

        return length;
    }

    //  Move construction may throw as far as the type system knows, so replacing it goes through the other member.

    struct throwing_move
    {
        throwing_move() {}
        throwing_move(const throwing_move &) {}
        throwing_move(throwing_move &&) {}
        throwing_move &operator=(const throwing_move &) { return *this; }
        throwing_move &operator=(throwing_move &&) { return *this; }
    };

    struct pinned
    {
        explicit pinned(uint32_t value)
            : value_(value)
        {
        }

        pinned(pinned &&) = delete;

        uint32_t value_;
    };

    using number = minstd::expected<uint32_t, parse_error>;

    static_assert(minstd::is_trivially_copy_constructible_v<number>);
    static_assert(minstd::is_trivially_move_constructible_v<number>);
    static_assert(minstd::is_trivially_destructible_v<number>);
    static_assert(!minstd::is_trivially_copy_constructible_v<minstd::expected<tracked, parse_error>>);
    static_assert(!minstd::is_copy_assignable_v<minstd::expected<throwing_move, throwing_move>>);
    static_assert(minstd::is_copy_assignable_v<minstd::expected<throwing_move, parse_error>>);
    static_assert(!minstd::is_default_constructible_v<minstd::expected<tracked, parse_error>>);
    static_assert(minstd::is_convertible_v<minstd::expected<uint16_t, parse_error>, number>);
    static_assert(minstd::is_same_v<number::rebind<bool>, minstd::expected<bool, parse_error>>);

    constexpr uint32_t evaluated_at_compile_time()
    {
        const number good(2u);
        const number bad(minstd::unexpect, parse_error::TOO_SHORT);
        const minstd::expected<void, parse_error> nothing;
        number copy = good;
        copy = good;

        return *copy.transform([](uint32_t value) { return value * 10; }) + good.value_or(0u) + bad.value_or(5u) +
               static_cast<uint32_t>(bad.error()) + (nothing.has_value() ? 100u : 0u) +
               (good == 2u ? 1000u : 0u) + (bad == minstd::unexpected(parse_error::TOO_SHORT) ? 10000u : 0u);
    }

    static_assert(evaluated_at_compile_time() == 11128u);

    TEST(ExpectedTests, HoldsAValueOrAnError)
    {
        const auto good = parse_length(24);
        const auto bad = parse_length(4);

        CHECK_TRUE(good.has_value());
        CHECK_TRUE(static_cast<bool>(good));
        CHECK_EQUAL(24U, good.value());
        CHECK_EQUAL(24U, *good);

        CHECK_FALSE(bad.has_value());
        CHECK_FALSE(static_cast<bool>(bad));
        CHECK_EQUAL(static_cast<int>(parse_error::TOO_SHORT), static_cast<int>(bad.error()));
    }

    TEST(ExpectedTests, ValueOrFallsBackOnlyForAnError)
    {
        CHECK_EQUAL(24U, parse_length(24).value_or(99U));
        CHECK_EQUAL(99U, parse_length(4).value_or(99U));
    }

    TEST(ExpectedTests, ArrowReachesTheValue)
    {
        const minstd::expected<tracked, parse_error> result(tracked(7));

        CHECK_EQUAL(7U, result->value());
    }

    TEST(ExpectedTests, ComparesWithValuesAndErrors)
    {
        CHECK_TRUE(parse_length(24) == 24U);
        CHECK_FALSE(parse_length(24) == 25U);
        CHECK_FALSE(parse_length(4) == 4U);
        CHECK_TRUE(parse_length(4) == minstd::unexpected(parse_error::TOO_SHORT));
        CHECK_FALSE(parse_length(4) == minstd::unexpected(parse_error::BAD_CHECKSUM));
        CHECK_FALSE(parse_length(24) == minstd::unexpected(parse_error::TOO_SHORT));
    }

    TEST(ExpectedTests, CopiesAndMovesKeepTheirState)
    {
        {
            const minstd::expected<tracked, parse_error> value(tracked(5));
            const minstd::expected<tracked, parse_error> error(minstd::unexpected(parse_error::BAD_CHECKSUM));

            minstd::expected<tracked, parse_error> value_copy(value);
            const minstd::expected<tracked, parse_error> error_copy(error);
            const minstd::expected<tracked, parse_error> moved(minstd::move(value_copy));

            CHECK_TRUE(value_copy.has_value()); //  a moved-from expected keeps its state, as std::expected's does
            CHECK_EQUAL(5U, moved->value());
            CHECK_FALSE(error_copy.has_value());
            CHECK_EQUAL(static_cast<int>(parse_error::BAD_CHECKSUM), static_cast<int>(error_copy.error()));
            CHECK_EQUAL(3, tracked::live);
        }

        CHECK_EQUAL(0, tracked::live);
    }

    TEST(ExpectedTests, AssignmentSwitchesStateAndDestroysTheOldOne)
    {
        {
            minstd::expected<tracked, parse_error> result(tracked(1));
            const minstd::expected<tracked, parse_error> error(minstd::unexpected(parse_error::TOO_SHORT));
            const minstd::expected<tracked, parse_error> other(tracked(2));

            result = error;
            CHECK_FALSE(result.has_value());
            CHECK_EQUAL(1, tracked::live);

            result = other;
            CHECK_EQUAL(2U, result->value());
            CHECK_EQUAL(2, tracked::live);

            result = minstd::expected<tracked, parse_error>(tracked(3));
            CHECK_EQUAL(3U, result->value());
            CHECK_EQUAL(2, tracked::live);

            result = result;
            CHECK_EQUAL(3U, result->value());
        }

        CHECK_EQUAL(0, tracked::live);
    }

    TEST(ExpectedTests, VoidExpectedIsSuccessOrAnError)
    {
        const minstd::expected<void, parse_error> success;
        const minstd::expected<void, parse_error> failure(minstd::unexpected(parse_error::BAD_CHECKSUM));

        CHECK_TRUE(success.has_value());
        CHECK_TRUE(static_cast<bool>(success));
        CHECK_FALSE(failure.has_value());
        CHECK_EQUAL(static_cast<int>(parse_error::BAD_CHECKSUM), static_cast<int>(failure.error()));
    }

    TEST(ExpectedTests, ConstructsInPlaceForEitherState)
    {
        const minstd::expected<tracked, tracked> value(minstd::in_place, 3u);
        const minstd::expected<tracked, tracked> error(minstd::unexpect, 4u);

        CHECK_EQUAL(3U, value->value());
        CHECK_EQUAL(4U, error.error().value());
    }

    TEST(ExpectedTests, ConvertsFromACompatibleExpected)
    {
        const minstd::expected<uint16_t, parse_error> small_value(uint16_t{7});
        const minstd::expected<uint16_t, parse_error> small_error(minstd::unexpect, parse_error::BAD_CHECKSUM);

        const number value(small_value);
        const number error(small_error);

        CHECK_EQUAL(7U, *value);
        CHECK_EQUAL(static_cast<int>(parse_error::BAD_CHECKSUM), static_cast<int>(error.error()));
    }

    TEST(ExpectedTests, ComparesWithAnotherExpected)
    {
        CHECK_TRUE(parse_length(24) == parse_length(24));
        CHECK_TRUE(parse_length(24) != parse_length(25));
        CHECK_TRUE(parse_length(4) == parse_length(5));
        CHECK_TRUE(parse_length(4) != parse_length(24));

        const minstd::expected<void, parse_error> success;
        const minstd::expected<void, parse_error> failure(minstd::unexpect, parse_error::TOO_SHORT);

        CHECK_TRUE(success == success);
        CHECK_TRUE(success != failure);
        CHECK_TRUE(failure == minstd::unexpected(parse_error::TOO_SHORT));
    }

    TEST(ExpectedTests, AssignsValuesErrorsAndEmplaces)
    {
        {
            minstd::expected<tracked, parse_error> result(tracked(1));

            result = minstd::unexpected(parse_error::TOO_SHORT);
            CHECK_FALSE(result.has_value());
            CHECK_EQUAL(0, tracked::live);

            result = tracked(2);
            CHECK_EQUAL(2U, result->value());
            CHECK_EQUAL(1, tracked::live);

            minstd::expected<uint32_t, parse_error> plain(minstd::unexpect, parse_error::TOO_SHORT);
            CHECK_EQUAL(9U, plain.emplace(9u));
            CHECK_TRUE(plain.has_value());
        }

        CHECK_EQUAL(0, tracked::live);
    }

    TEST(ExpectedTests, SwapExchangesEveryCombinationOfStates)
    {
        {
            minstd::expected<tracked, parse_error> first(tracked(1));
            minstd::expected<tracked, parse_error> second(minstd::unexpect, parse_error::BAD_CHECKSUM);

            swap(first, second);
            CHECK_FALSE(first.has_value());
            CHECK_EQUAL(1U, second->value());
            CHECK_EQUAL(1, tracked::live);

            first.swap(second);
            CHECK_EQUAL(1U, first->value());
            CHECK_EQUAL(static_cast<int>(parse_error::BAD_CHECKSUM), static_cast<int>(second.error()));

            minstd::expected<tracked, parse_error> third(tracked(3));
            first.swap(third);
            CHECK_EQUAL(3U, first->value());
            CHECK_EQUAL(1U, third->value());

            minstd::expected<void, parse_error> success;
            minstd::expected<void, parse_error> failure(minstd::unexpect, parse_error::TOO_SHORT);
            swap(success, failure);
            CHECK_FALSE(success.has_value());
            CHECK_TRUE(failure.has_value());
        }

        CHECK_EQUAL(0, tracked::live);
    }

    TEST(ExpectedTests, RvalueAccessMovesOut)
    {
        minstd::expected<tracked, parse_error> result(tracked(6));

        const tracked taken(minstd::move(result).value());

        CHECK_EQUAL(6U, taken.value());
        CHECK_TRUE(result.has_value());
        CHECK_EQUAL(0U, result->value());
    }

    TEST(ExpectedTests, ErrorOrFallsBackOnlyForAValue)
    {
        CHECK_EQUAL(static_cast<int>(parse_error::TOO_SHORT),
                    static_cast<int>(parse_length(4).error_or(parse_error::BAD_CHECKSUM)));
        CHECK_EQUAL(static_cast<int>(parse_error::BAD_CHECKSUM),
                    static_cast<int>(parse_length(24).error_or(parse_error::BAD_CHECKSUM)));
    }

    TEST(ExpectedTests, AndThenAndTransformRunOnlyForAValue)
    {
        int calls = 0;
        const auto doubled = [&calls](uint32_t value) {
            calls++;
            return value * 2;
        };

        CHECK_EQUAL(48U, *parse_length(24).transform(doubled));
        CHECK_EQUAL(30U, *parse_length(24).and_then([](uint32_t value) { return parse_length(value + 6); }));
        CHECK_TRUE(parse_length(4).transform(doubled) == minstd::unexpected(parse_error::TOO_SHORT));
        CHECK_TRUE(parse_length(4).and_then(parse_length) == minstd::unexpected(parse_error::TOO_SHORT));
        CHECK_EQUAL(1, calls);

        CHECK_TRUE(parse_length(24).transform([](uint32_t) {}).has_value());
        CHECK_EQUAL(24U, parse_length(24).transform([](uint32_t value) { return pinned(value); })->value_);
    }

    TEST(ExpectedTests, OrElseAndTransformErrorRunOnlyForAnError)
    {
        const auto recover = [](parse_error) { return number(20u); };
        const auto code = [](parse_error error) { return static_cast<uint32_t>(error); };

        CHECK_EQUAL(20U, *parse_length(4).or_else(recover));
        CHECK_EQUAL(24U, *parse_length(24).or_else(recover));
        CHECK_EQUAL(1U, parse_length(4).transform_error(code).error());
        CHECK_EQUAL(24U, *parse_length(24).transform_error(code));
    }

    TEST(ExpectedTests, VoidExpectedChains)
    {
        const minstd::expected<void, parse_error> success;
        const minstd::expected<void, parse_error> failure(minstd::unexpect, parse_error::TOO_SHORT);

        CHECK_EQUAL(5U, *success.transform([] { return 5u; }));
        CHECK_EQUAL(24U, *success.and_then([] { return parse_length(24); }));
        CHECK_FALSE(failure.transform([] { return 5u; }).has_value());
        CHECK_TRUE(failure.or_else([](parse_error) { return minstd::expected<void, parse_error>(); }).has_value());
        CHECK_EQUAL(1U, failure.transform_error([](parse_error error) { return static_cast<uint32_t>(error); }).error());
    }

    TEST(ExpectedTests, UnexpectedSwapsAndCompares)
    {
        minstd::unexpected<uint32_t> first(1u);
        minstd::unexpected<uint32_t> second(minstd::in_place, 2u);

        swap(first, second);

        CHECK_EQUAL(2U, first.error());
        CHECK_TRUE(first != second);
        CHECK_TRUE(second == minstd::unexpected(1u));
    }
}
