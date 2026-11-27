// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <stdint.h>

#include <minimalcstdlib.h>

#include <result>

#include <__memory_resource/memory_resource.h>

namespace
{
    enum class fetch_code : uint32_t
    {
        SUCCESS = 0,
        EMPTY,
        NO_MEMORY,
    };

    int Code(fetch_code code)
    {
        return static_cast<int>(code);
    }

    int Code(minstd::simple_success_or_failure code)
    {
        return static_cast<int>(code);
    }

    //  Counts its live instances, so tests can see that each state is constructed and destroyed exactly once.

    class tracked
    {
    public:
        static inline int live = 0;

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

    class counting_resource : public minstd::pmr::memory_resource
    {
    public:
        int allocations_ = 0;
        int deallocations_ = 0;

    private:
        void *do_allocate(size_t bytes, size_t) override
        {
            allocations_++;
            return malloc(bytes);
        }

        void do_deallocate(void *ptr, size_t, size_t) override
        {
            deallocations_++;
            free(ptr);
        }

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override
        {
            return this == &other;
        }
    };

    struct payload
    {
        uint32_t value;
    };

    static_assert(minstd::result_code_enum<fetch_code>);
    static_assert(minstd::result_code_enum<minstd::simple_success_or_failure>);
    static_assert(!minstd::result_code_enum<uint32_t>);

    //  status_result and simple_result

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(StatusResultTests)
    {};
#pragma GCC diagnostic pop

    minstd::simple_result Check(bool ok)
    {
        if (!ok)
        {
            return minstd::unexpected(minstd::simple_success_or_failure::FAILURE);
        }

        return {};
    }

    TEST(StatusResultTests, SimpleSuccessOrFailureIsZeroForSuccess)
    {
        CHECK_EQUAL(0, Code(minstd::simple_success_or_failure::SUCCESS));
        CHECK(minstd::simple_success_or_failure::SUCCESS != minstd::simple_success_or_failure::FAILURE);
    }

    TEST(StatusResultTests, SuccessAndFailureReportTheirCodes)
    {
        const minstd::simple_result success = Check(true);
        const minstd::simple_result failure = Check(false);

        CHECK_TRUE(success.has_value());
        CHECK_TRUE(static_cast<bool>(success));
        CHECK_EQUAL(Code(minstd::simple_success_or_failure::SUCCESS), Code(success.result_code()));

        CHECK_FALSE(failure.has_value());
        CHECK_EQUAL(Code(minstd::simple_success_or_failure::FAILURE), Code(failure.result_code()));
        CHECK_EQUAL(Code(minstd::simple_success_or_failure::FAILURE), Code(failure.error()));
    }

    TEST(StatusResultTests, AnyCodeEnumNamesTheFailure)
    {
        const minstd::status_result<fetch_code> success;
        const minstd::status_result<fetch_code> no_memory = minstd::unexpected(fetch_code::NO_MEMORY);

        CHECK_EQUAL(Code(fetch_code::SUCCESS), Code(success.result_code()));
        CHECK_EQUAL(Code(fetch_code::NO_MEMORY), Code(no_memory.result_code()));
    }

    TEST(StatusResultTests, CopiesKeepTheirState)
    {
        const minstd::simple_result failure = Check(false);
        minstd::simple_result copy = failure;

        CHECK_FALSE(copy.has_value());
        CHECK_EQUAL(Code(minstd::simple_success_or_failure::FAILURE), Code(copy.result_code()));

        copy = Check(true);
        CHECK_TRUE(copy.has_value());
    }

    //  value_result

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(ValueResultTests)
    {};
#pragma GCC diagnostic pop

    using tracked_result = minstd::value_result<fetch_code, tracked>;

    tracked_result Make(fetch_code code, uint32_t value)
    {
        if (code != fetch_code::SUCCESS)
        {
            return minstd::unexpected(code);
        }

        return tracked(value);
    }

    TEST(ValueResultTests, AValueReportsSuccess)
    {
        const minstd::value_result<fetch_code, uint32_t> result = 42u;

        CHECK_TRUE(result.has_value());
        CHECK_EQUAL(Code(fetch_code::SUCCESS), Code(result.result_code()));
        CHECK_EQUAL(42u, *result);
        CHECK_EQUAL(42u, result.value());
    }

    TEST(ValueResultTests, AFailureReportsItsCodeAndHoldsNoValue)
    {
        tracked::live = 0;

        {
            const tracked_result result = Make(fetch_code::EMPTY, 0);

            CHECK_FALSE(result.has_value());
            CHECK_EQUAL(Code(fetch_code::EMPTY), Code(result.result_code()));
            CHECK_EQUAL(Code(fetch_code::EMPTY), Code(result.error()));
            CHECK_EQUAL(0, tracked::live);
        }

        CHECK_EQUAL(0, tracked::live);
    }

    TEST(ValueResultTests, CopiesAndMovesConstructAndDestroyEachValueOnce)
    {
        tracked::live = 0;

        {
            tracked_result first = Make(fetch_code::SUCCESS, 7);
            tracked_result copy = first;
            tracked_result moved(minstd::move(first));

            CHECK_EQUAL(7u, copy->value());
            CHECK_EQUAL(7u, moved->value());
            CHECK_EQUAL(3, tracked::live);

            copy = Make(fetch_code::NO_MEMORY, 0);
            CHECK_EQUAL(Code(fetch_code::NO_MEMORY), Code(copy.result_code()));
            CHECK_EQUAL(2, tracked::live);
        }

        CHECK_EQUAL(0, tracked::live);
    }

    TEST(ValueResultTests, ACopiedBooleanResultKeepsItsValueNotItsState)
    {
        minstd::value_result<fetch_code, bool> original = false;
        const minstd::value_result<fetch_code, bool> copy = original;
        minstd::value_result<fetch_code, bool> assigned = true;
        assigned = original;

        CHECK_TRUE(copy.has_value());
        CHECK_FALSE(*copy);
        CHECK_FALSE(*assigned);
    }

    TEST(ValueResultTests, ResultsCompareByStateAndContent)
    {
        using number_result = minstd::value_result<fetch_code, uint32_t>;

        const number_result seven = 7u;
        const number_result empty = minstd::unexpected(fetch_code::EMPTY);

        CHECK_TRUE(seven == number_result(7u));
        CHECK_TRUE(seven != empty);
        CHECK_TRUE(empty == number_result(minstd::unexpected(fetch_code::EMPTY)));
        CHECK_TRUE(seven == 7u);
        CHECK_TRUE(empty == minstd::unexpected(fetch_code::EMPTY));
    }

    //  value_result_with_error_info

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(ValueResultWithErrorInfoTests)
    {};
#pragma GCC diagnostic pop

    struct position
    {
        uint32_t line;
        uint32_t column;
    };

    using parse_result = minstd::value_result_with_error_info<fetch_code, uint32_t, position>;

    parse_result Parse(fetch_code code, bool with_position)
    {
        if (code == fetch_code::SUCCESS)
        {
            return 99u;
        }

        if (with_position)
        {
            return {minstd::unexpected(code), position{3, 14}};
        }

        return minstd::unexpected(code);
    }

    TEST(ValueResultWithErrorInfoTests, ASuccessHasAValueAndNoErrorInfo)
    {
        const parse_result result = Parse(fetch_code::SUCCESS, false);

        CHECK_TRUE(result.has_value());
        CHECK_EQUAL(Code(fetch_code::SUCCESS), Code(result.result_code()));
        CHECK_EQUAL(99u, *result);
        CHECK_FALSE(result.error_info().has_value());
    }

    TEST(ValueResultWithErrorInfoTests, AFailureCarriesItsCodeWithOrWithoutInfo)
    {
        const parse_result bare = Parse(fetch_code::EMPTY, false);
        const parse_result located = Parse(fetch_code::NO_MEMORY, true);

        CHECK_FALSE(bare.has_value());
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(bare.result_code()));
        CHECK_FALSE(bare.error_info().has_value());

        CHECK_FALSE(located.has_value());
        CHECK_EQUAL(Code(fetch_code::NO_MEMORY), Code(located.result_code()));
        CHECK_TRUE(located.error_info().has_value());
        CHECK_EQUAL(3u, located.error_info()->line);
        CHECK_EQUAL(14u, located.error_info()->column);
    }

    //  reference_result

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(ReferenceResultTests)
    {};
#pragma GCC diagnostic pop

    struct base_entry
    {
        uint32_t id;
    };

    struct derived_entry : base_entry
    {
        uint32_t extra;
    };

    payload table[4] = {{10}, {11}, {12}, {13}};

    minstd::reference_result<fetch_code, payload> Find(uint32_t index)
    {
        if (index >= 4)
        {
            return minstd::unexpected(fetch_code::EMPTY);
        }

        return table[index];
    }

    TEST(ReferenceResultTests, AReferenceReportsSuccessAndIsTheObjectItself)
    {
        const minstd::reference_result<fetch_code, payload> result = Find(2);

        CHECK_TRUE(result.has_value());
        CHECK_EQUAL(Code(fetch_code::SUCCESS), Code(result.result_code()));
        POINTERS_EQUAL(&table[2], &result.value());
        POINTERS_EQUAL(&table[2], &*result);
        CHECK_EQUAL(12u, result->value);

        result->value = 120;
        CHECK_EQUAL(120u, table[2].value);
        table[2].value = 12;
    }

    TEST(ReferenceResultTests, AFailureReportsItsCode)
    {
        const minstd::reference_result<fetch_code, payload> result = Find(9);

        CHECK_FALSE(result.has_value());
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(result.result_code()));
    }

    TEST(ReferenceResultTests, CopiesReferToTheSameObjectAndADerivedObjectBindsToItsBase)
    {
        derived_entry entry{{5}, 6};
        const minstd::reference_result<fetch_code, base_entry> result = entry;
        const minstd::reference_result<fetch_code, base_entry> copy = result;
        const minstd::reference_result<fetch_code, const payload> read_only = table[0];

        POINTERS_EQUAL(static_cast<base_entry *>(&entry), &*result);
        POINTERS_EQUAL(&*result, &*copy);
        CHECK_EQUAL(10u, read_only->value);
    }

    //  pointer_result

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(PointerResultTests)
    {};
#pragma GCC diagnostic pop

    using fetch_result = minstd::pointer_result<fetch_code, payload>;

    fetch_result Fetch(counting_resource &resource, fetch_code code, uint32_t value)
    {
        if (code != fetch_code::SUCCESS)
        {
            return minstd::unexpected(code);
        }

        minstd::unique_ptr<payload> owned(new (resource.allocate(sizeof(payload), alignof(payload))) payload{value}, resource);

        return minstd::move(owned);
    }

    TEST(PointerResultTests, AValueReportsSuccessAndOwnsTheObject)
    {
        counting_resource resource;

        {
            fetch_result result = Fetch(resource, fetch_code::SUCCESS, 42);

            CHECK_TRUE(result.has_value());
            CHECK_TRUE(static_cast<bool>(result));
            CHECK_EQUAL(Code(fetch_code::SUCCESS), Code(result.result_code()));
            CHECK_EQUAL(42u, (*result)->value);
            CHECK_EQUAL(42u, result.value()->value);
            CHECK_EQUAL(0, resource.deallocations_);
        }

        CHECK_EQUAL(1, resource.allocations_);
        CHECK_EQUAL(1, resource.deallocations_);
    }

    TEST(PointerResultTests, AnErrorReportsItsCodeAndHoldsNothing)
    {
        counting_resource resource;

        const fetch_result empty = Fetch(resource, fetch_code::EMPTY, 0);
        const fetch_result no_memory = Fetch(resource, fetch_code::NO_MEMORY, 0);

        CHECK_FALSE(empty.has_value());
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(empty.result_code()));
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(empty.error()));
        CHECK_FALSE(no_memory.has_value());
        CHECK_EQUAL(Code(fetch_code::NO_MEMORY), Code(no_memory.result_code()));
        CHECK_EQUAL(0, resource.allocations_);
    }

    TEST(PointerResultTests, MovingPassesOwnershipAndFreesExactlyOnce)
    {
        counting_resource resource;

        {
            fetch_result first = Fetch(resource, fetch_code::SUCCESS, 7);
            fetch_result second(minstd::move(first));

            CHECK_EQUAL(7u, (*second)->value);

            //  Assigning over a value frees it; a loop that receives until empty relies on this.

            second = Fetch(resource, fetch_code::SUCCESS, 8);
            CHECK_EQUAL(1, resource.deallocations_);
            CHECK_EQUAL(8u, (*second)->value);

            second = Fetch(resource, fetch_code::EMPTY, 0);
            CHECK_EQUAL(2, resource.deallocations_);
            CHECK_EQUAL(Code(fetch_code::EMPTY), Code(second.result_code()));
        }

        CHECK_EQUAL(2, resource.allocations_);
        CHECK_EQUAL(2, resource.deallocations_);
    }

    TEST(PointerResultTests, TakingTheObjectLeavesTheResultToFreeNothing)
    {
        counting_resource resource;

        {
            fetch_result result = Fetch(resource, fetch_code::SUCCESS, 9);
            minstd::unique_ptr<payload> taken(minstd::move(*result));

            CHECK_EQUAL(9u, taken->value);
            CHECK_FALSE(static_cast<bool>(*result));
            CHECK_EQUAL(Code(fetch_code::SUCCESS), Code(result.result_code()));
        }

        CHECK_EQUAL(1, resource.deallocations_);
    }
    //  Chaining

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(ResultChainingTests)
    {};
#pragma GCC diagnostic pop

    using number_result = minstd::value_result<fetch_code, uint32_t>;

    number_result Number(fetch_code code, uint32_t value)
    {
        if (code != fetch_code::SUCCESS)
        {
            return minstd::unexpected(code);
        }

        return value;
    }

    TEST(ResultChainingTests, AndThenPassesAValueOnAndAnErrorThrough)
    {
        int calls = 0;
        const auto find = [&calls](uint32_t index) {
            calls++;
            return Find(index);
        };

        const auto found = Number(fetch_code::SUCCESS, 1).and_then(find);
        const auto missing = Number(fetch_code::SUCCESS, 9).and_then(find);
        const auto skipped = Number(fetch_code::NO_MEMORY, 1).and_then(find);

        POINTERS_EQUAL(&table[1], &found.value());
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(missing.result_code()));
        CHECK_EQUAL(Code(fetch_code::NO_MEMORY), Code(skipped.result_code()));
        CHECK_EQUAL(2, calls);
    }

    TEST(ResultChainingTests, AndThenChainsFromEveryResultType)
    {
        counting_resource resource;
        const auto fetch = [&resource](uint32_t value) { return Fetch(resource, fetch_code::SUCCESS, value); };
        const auto read = [](const minstd::unique_ptr<payload> &owned) {
            return Number(fetch_code::SUCCESS, owned->value);
        };
        const auto check = [](payload &entry) {
            return entry.value > 11 ? minstd::status_result<fetch_code>()
                                    : minstd::status_result<fetch_code>(minstd::unexpected(fetch_code::EMPTY));
        };
        const auto next = [] { return Number(fetch_code::SUCCESS, 5); };

        CHECK_EQUAL(12u, *Find(2).and_then([&](payload &entry) { return fetch(entry.value); }).and_then(read));
        CHECK_EQUAL(5u, *Find(2).and_then(check).and_then(next));
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(Find(0).and_then(check).and_then(next).result_code()));
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(Find(9).and_then(check).result_code()));
        CHECK_EQUAL(1, resource.allocations_);
        CHECK_EQUAL(1, resource.deallocations_);
    }

    TEST(ResultChainingTests, AndThenKeepsTheErrorInformation)
    {
        const auto halve = [](uint32_t value) { return parse_result(value / 2); };

        const parse_result located = Parse(fetch_code::NO_MEMORY, true).and_then(halve);

        CHECK_EQUAL(49u, *Parse(fetch_code::SUCCESS, false).and_then(halve));
        CHECK_EQUAL(Code(fetch_code::NO_MEMORY), Code(located.result_code()));
        CHECK_EQUAL(14u, located.error_info()->column);
    }

    TEST(ResultChainingTests, OrElseRecoversOrChangesTheCode)
    {
        const auto fallback = [](fetch_code) { return Find(3); };
        const auto simplify = [](fetch_code) {
            return minstd::value_result<minstd::simple_success_or_failure, uint32_t>(
                minstd::unexpected(minstd::simple_success_or_failure::FAILURE));
        };
        const auto recover = [](fetch_code) { return minstd::status_result<fetch_code>(); };

        POINTERS_EQUAL(&table[1], &Find(1).or_else(fallback).value());
        POINTERS_EQUAL(&table[3], &Find(9).or_else(fallback).value());
        CHECK_EQUAL(8u, *Number(fetch_code::SUCCESS, 8).or_else(simplify));
        CHECK_EQUAL(Code(minstd::simple_success_or_failure::FAILURE),
                    Code(Number(fetch_code::EMPTY, 0).or_else(simplify).result_code()));
        CHECK_TRUE(minstd::status_result<fetch_code>(minstd::unexpected(fetch_code::EMPTY)).or_else(recover).has_value());
        CHECK_TRUE(minstd::status_result<fetch_code>().or_else(recover).has_value());
    }

    TEST(ResultChainingTests, OrElseMovesAnOwnedValueThrough)
    {
        counting_resource resource;
        const auto retry = [&resource](fetch_code) { return Fetch(resource, fetch_code::SUCCESS, 2); };

        {
            const fetch_result kept = Fetch(resource, fetch_code::SUCCESS, 1).or_else(retry);
            const fetch_result retried = Fetch(resource, fetch_code::EMPTY, 0).or_else(retry);

            CHECK_EQUAL(1u, (*kept)->value);
            CHECK_EQUAL(2u, (*retried)->value);
            CHECK_EQUAL(0, resource.deallocations_);
        }

        CHECK_EQUAL(2, resource.deallocations_);
    }
    TEST(ResultChainingTests, AReferenceResultPassesTheObjectItself)
    {
        const auto same = [](auto &entry) { return minstd::reference_result<fetch_code, payload>(entry); };
        const auto address = [](auto &entry) { return &entry; };

        POINTERS_EQUAL(&table[2], &Find(2).and_then(same).value());
        POINTERS_EQUAL(&table[2], *Find(2).transform(address));
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(Find(9).and_then(same).result_code()));
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(Find(9).transform(address).error()));

        const auto lookup = Find(1);
        CHECK_EQUAL(11u, *lookup.transform([](const payload &entry) { return entry.value; }));
    }

    //  Each of these returns the plain expected that transform or transform_error made as a result type.

    minstd::value_result<fetch_code, uint32_t> Doubled(uint32_t value)
    {
        return Number(fetch_code::SUCCESS, value).transform([](uint32_t number) { return number * 2; });
    }

    minstd::status_result<fetch_code> Discarded(fetch_code code)
    {
        return Number(code, 1).transform([](uint32_t) {});
    }

    minstd::value_result<minstd::simple_success_or_failure, uint32_t> Simplified(fetch_code code)
    {
        return Number(code, 6).transform_error([](fetch_code) { return minstd::simple_success_or_failure::FAILURE; });
    }

    minstd::reference_result<fetch_code, uint32_t> ValueOf(uint32_t index)
    {
        return Find(index).transform([](payload &entry) { return minstd::reference_wrapper<uint32_t>(entry.value); });
    }

    parse_result Halved(fetch_code code)
    {
        return Parse(code, true).transform([](uint32_t value) { return value / 2; });
    }

    TEST(ResultChainingTests, ATransformedResultReturnsAsAResult)
    {
        CHECK_EQUAL(8u, *Doubled(4));
        CHECK_TRUE(Discarded(fetch_code::SUCCESS).has_value());
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(Discarded(fetch_code::EMPTY).result_code()));
        CHECK_EQUAL(6u, *Simplified(fetch_code::SUCCESS));
        CHECK_EQUAL(Code(minstd::simple_success_or_failure::FAILURE), Code(Simplified(fetch_code::EMPTY).result_code()));
        POINTERS_EQUAL(&table[2].value, &ValueOf(2).value());
        CHECK_EQUAL(Code(fetch_code::EMPTY), Code(ValueOf(9).result_code()));
        CHECK_EQUAL(49u, *Halved(fetch_code::SUCCESS));
        CHECK_EQUAL(14u, Halved(fetch_code::NO_MEMORY).error_info()->column);
    }

    TEST(ResultChainingTests, ATransformedResultOwnsWhatItWasGiven)
    {
        counting_resource resource;

        {
            const fetch_result result = Fetch(resource, fetch_code::SUCCESS, 3)
                                            .transform([](minstd::unique_ptr<payload> &&owned) {
                                                return minstd::move(owned);
                                            });

            CHECK_EQUAL(3u, (*result)->value);
            CHECK_EQUAL(0, resource.deallocations_);
        }

        CHECK_EQUAL(1, resource.deallocations_);
    }

    static_assert(!minstd::is_copy_constructible_v<fetch_result>);
    static_assert(!minstd::is_constructible_v<fetch_result, const minstd::expected<minstd::unique_ptr<payload>, fetch_code> &>);
    static_assert(minstd::is_constructible_v<fetch_result, minstd::expected<minstd::unique_ptr<payload>, fetch_code> &&>);
} // namespace
