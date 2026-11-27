// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <stdint.h>

#include <atomic>
#include <limits>
#include <statistics_table>
#include <type_traits>

#include <pthread.h>

namespace
{
    enum class test_counter : uint32_t
    {
        FRAMES = 0,
        ERRORS,
        DROPS,
        COUNT
    };

    enum class single_counter : uint32_t
    {
        ONLY = 0,
        COUNT
    };

    enum class out_of_order_counter : uint32_t
    {
        FIRST = 0,
        SECOND,
        COUNT
    };

    enum class missing_descriptor_counter : uint32_t
    {
        FIRST = 0,
        SECOND,
        COUNT
    };

    enum class unnamed_descriptor_counter : uint32_t
    {
        FIRST = 0,
        SECOND,
        COUNT
    };

    enum class no_count_enum : uint32_t
    {
        FIRST = 0,
    };

    enum class no_traits_enum : uint32_t
    {
        FIRST = 0,
        COUNT
    };
}

namespace minstd
{
    template <>
    struct statistics_traits<test_counter>
    {
        static constexpr array<statistic_descriptor<test_counter>, 3> DESCRIPTORS = {{
            {test_counter::FRAMES, "frames", "Frames seen"},
            {test_counter::ERRORS, "errors", "Frames with errors"},
            {test_counter::DROPS, "drops", "Frames dropped"},
        }};
    };

    template <>
    struct statistics_traits<single_counter>
    {
        static constexpr array<statistic_descriptor<single_counter>, 1> DESCRIPTORS = {{
            {single_counter::ONLY, "only", "The one statistic"},
        }};
    };

    template <>
    struct statistics_traits<out_of_order_counter>
    {
        static constexpr array<statistic_descriptor<out_of_order_counter>, 2> DESCRIPTORS = {{
            {out_of_order_counter::SECOND, "second", ""},
            {out_of_order_counter::FIRST, "first", ""},
        }};
    };

    template <>
    struct statistics_traits<missing_descriptor_counter>
    {
        static constexpr array<statistic_descriptor<missing_descriptor_counter>, 1> DESCRIPTORS = {{
            {missing_descriptor_counter::FIRST, "first", ""},
        }};
    };

    template <>
    struct statistics_traits<unnamed_descriptor_counter>
    {
        static constexpr array<statistic_descriptor<unnamed_descriptor_counter>, 2> DESCRIPTORS = {{
            {unnamed_descriptor_counter::FIRST, "first", ""},
            {unnamed_descriptor_counter::SECOND, "", ""},
        }};
    };

    template <>
    struct statistics_traits<no_count_enum>
    {
        static constexpr array<statistic_descriptor<no_count_enum>, 1> DESCRIPTORS = {{
            {no_count_enum::FIRST, "first", ""},
        }};
    };
}

namespace
{
    using test_table = minstd::statistics_table<test_counter>;

    constexpr minstd::array<test_counter, 3> ALL_COUNTERS = {test_counter::FRAMES, test_counter::ERRORS, test_counter::DROPS};

    //  Compile-time contract

    static_assert(minstd::statistics_enum<test_counter>);
    static_assert(!minstd::statistics_enum<uint32_t>, "not an enum");
    static_assert(!minstd::statistics_enum<no_count_enum>, "no COUNT");
    static_assert(!minstd::statistics_enum<no_traits_enum>, "no statistics_traits");

    static_assert(minstd::descriptors_in_enum_order<test_counter>());
    static_assert(!minstd::descriptors_in_enum_order<out_of_order_counter>());
    static_assert(!minstd::descriptors_in_enum_order<missing_descriptor_counter>());
    static_assert(!minstd::descriptors_in_enum_order<unnamed_descriptor_counter>());

    static_assert(test_table::SIZE == 3);
    static_assert(minstd::statistics_table<single_counter>::SIZE == 1);

    static_assert(!minstd::is_copy_constructible_v<test_table>);
    static_assert(!minstd::is_copy_assignable_v<test_table>);

    static_assert(test_table::describe(test_counter::ERRORS).name_ == minstd::string_view("errors"));
    static_assert(test_table::describe(test_counter::DROPS).which_ == test_counter::DROPS);

    constinit test_table constant_initialized_table;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(StatisticsTableValueTests)
    {};

    TEST_GROUP(StatisticsTableDescriptorTests)
    {};

    TEST_GROUP(StatisticsTableIterationTests)
    {};

    TEST_GROUP(StatisticsTableConcurrencyTests)
    {};
#pragma GCC diagnostic pop

    //  Values

    TEST(StatisticsTableValueTests, EveryValueStartsAtZero)
    {
        test_table table;

        for (test_counter which : ALL_COUNTERS)
        {
            CHECK_EQUAL(0, table.value(which));
            CHECK_EQUAL(0, constant_initialized_table.value(which));
        }
    }

    TEST(StatisticsTableValueTests, IncrementAddsOneToOnlyItsOwnEntry)
    {
        test_table table;

        table.increment(test_counter::ERRORS);

        CHECK_EQUAL(0, table.value(test_counter::FRAMES));
        CHECK_EQUAL(1, table.value(test_counter::ERRORS));
        CHECK_EQUAL(0, table.value(test_counter::DROPS));

        table.increment(test_counter::ERRORS);
        table.increment(test_counter::DROPS);

        CHECK_EQUAL(0, table.value(test_counter::FRAMES));
        CHECK_EQUAL(2, table.value(test_counter::ERRORS));
        CHECK_EQUAL(1, table.value(test_counter::DROPS));
    }

    TEST(StatisticsTableValueTests, IncrementByAnAmountAddsThatAmount)
    {
        test_table table;

        table.increment(test_counter::FRAMES, 40);
        table.increment(test_counter::FRAMES, 2);
        CHECK_EQUAL(42, table.value(test_counter::FRAMES));

        table.increment(test_counter::FRAMES, 0);
        CHECK_EQUAL(42, table.value(test_counter::FRAMES));

        table.increment(test_counter::DROPS, 0x100000000ULL);
        CHECK_EQUAL(0x100000000ULL, table.value(test_counter::DROPS));
    }

    TEST(StatisticsTableValueTests, ValueWrapsAroundPastTheMaximum)
    {
        test_table table;

        table.increment(test_counter::ERRORS, minstd::numeric_limits<uint64_t>::max());
        CHECK_EQUAL(minstd::numeric_limits<uint64_t>::max(), table.value(test_counter::ERRORS));

        table.increment(test_counter::ERRORS);
        CHECK_EQUAL(0, table.value(test_counter::ERRORS));
    }

    TEST(StatisticsTableValueTests, TablesAreIndependent)
    {
        test_table first;
        test_table second;

        first.increment(test_counter::FRAMES, 5);
        second.increment(test_counter::FRAMES, 7);

        CHECK_EQUAL(5, first.value(test_counter::FRAMES));
        CHECK_EQUAL(7, second.value(test_counter::FRAMES));
    }

    TEST(StatisticsTableValueTests, SingleEntryTableCounts)
    {
        minstd::statistics_table<single_counter> table;

        table.increment(single_counter::ONLY);
        table.increment(single_counter::ONLY, 9);

        CHECK_EQUAL(10, table.value(single_counter::ONLY));
    }

    //  Descriptors

    TEST(StatisticsTableDescriptorTests, DescribeReturnsEachValuesDescriptor)
    {
        CHECK_TRUE(test_table::describe(test_counter::FRAMES).name_ == minstd::string_view("frames"));
        CHECK_TRUE(test_table::describe(test_counter::ERRORS).name_ == minstd::string_view("errors"));
        CHECK_TRUE(test_table::describe(test_counter::DROPS).name_ == minstd::string_view("drops"));

        CHECK_TRUE(test_table::describe(test_counter::FRAMES).description_ == minstd::string_view("Frames seen"));
        CHECK_TRUE(test_table::describe(test_counter::ERRORS).description_ == minstd::string_view("Frames with errors"));
        CHECK_TRUE(test_table::describe(test_counter::DROPS).description_ == minstd::string_view("Frames dropped"));

        for (test_counter which : ALL_COUNTERS)
        {
            CHECK_EQUAL(static_cast<uint32_t>(which), static_cast<uint32_t>(test_table::describe(which).which_));
        }
    }

    TEST(StatisticsTableDescriptorTests, DescribeIsTheTraitsDescriptor)
    {
        for (test_counter which : ALL_COUNTERS)
        {
            POINTERS_EQUAL(&minstd::statistics_traits<test_counter>::DESCRIPTORS[static_cast<size_t>(which)],
                           &test_table::describe(which));
        }
    }

    TEST(StatisticsTableDescriptorTests, IncrementingDoesNotChangeDescriptors)
    {
        test_table table;

        table.increment(test_counter::FRAMES, 100);

        CHECK_TRUE(test_table::describe(test_counter::FRAMES).name_ == minstd::string_view("frames"));
        CHECK_TRUE(test_table::describe(test_counter::FRAMES).description_ == minstd::string_view("Frames seen"));
    }

    //  Iteration

    TEST(StatisticsTableIterationTests, ForEachVisitsEveryEntryOnceInEnumOrder)
    {
        test_table table;

        table.increment(test_counter::FRAMES, 3);
        table.increment(test_counter::DROPS, 8);

        uint32_t visits = 0;

        table.for_each([&](const test_table::descriptor_type &descriptor, uint64_t value) {
            CHECK_EQUAL(visits, static_cast<uint32_t>(descriptor.which_));
            CHECK_EQUAL(table.value(descriptor.which_), value);
            visits++;
        });

        CHECK_EQUAL(test_table::SIZE, visits);
    }

    TEST(StatisticsTableIterationTests, ForEachReportsCurrentValues)
    {
        test_table table;
        minstd::array<uint64_t, 3> seen{};

        table.increment(test_counter::FRAMES, 3);
        table.increment(test_counter::ERRORS, 1);
        table.increment(test_counter::DROPS, 8);

        const test_table &read_only = table;

        read_only.for_each([&](const auto &descriptor, uint64_t value) { seen[static_cast<size_t>(descriptor.which_)] = value; });

        CHECK_EQUAL(3, seen[0]);
        CHECK_EQUAL(1, seen[1]);
        CHECK_EQUAL(8, seen[2]);

        table.increment(test_counter::ERRORS, 4);

        read_only.for_each([&](const auto &descriptor, uint64_t value) { seen[static_cast<size_t>(descriptor.which_)] = value; });

        CHECK_EQUAL(3, seen[0]);
        CHECK_EQUAL(5, seen[1]);
        CHECK_EQUAL(8, seen[2]);
    }

    TEST(StatisticsTableIterationTests, ForEachPassesTheTraitsDescriptors)
    {
        test_table table;

        table.for_each([](const test_table::descriptor_type &descriptor, uint64_t) {
            POINTERS_EQUAL(&test_table::describe(descriptor.which_), &descriptor);
        });
    }

    //  Concurrency: one writer per table, any number of readers.

    constexpr uint32_t WRITES = 200000;
    constexpr uint32_t READERS = 3;

    //  Every value the writer leaves has equal halves, so a torn read shows unequal ones.
    constexpr uint64_t TEAR_DETECTING_STEP = 0x0000000100000001ULL;

    struct shared_state
    {
        test_table                  table_;
        minstd::atomic<uint32_t>    writer_done_{0};
        minstd::atomic<uint32_t>    readers_started_{0};
        minstd::atomic<uint32_t>    failures_{0};
    };

    void wait_for_readers(shared_state &state)
    {
        while (state.readers_started_.load(minstd::memory_order_acquire) < READERS)
        {
        }
    }

    void *write_one_entry(void *argument)
    {
        shared_state &state = *static_cast<shared_state *>(argument);

        wait_for_readers(state);

        for (uint32_t i = 0; i < WRITES; i++)
        {
            state.table_.increment(test_counter::FRAMES);
        }

        state.writer_done_.store(1, minstd::memory_order_release);
        return nullptr;
    }

    void *write_tear_detecting_steps(void *argument)
    {
        shared_state &state = *static_cast<shared_state *>(argument);

        wait_for_readers(state);

        for (uint32_t i = 0; i < WRITES; i++)
        {
            state.table_.increment(test_counter::ERRORS, TEAR_DETECTING_STEP);
        }

        state.writer_done_.store(1, minstd::memory_order_release);
        return nullptr;
    }

    void *write_every_entry_in_turn(void *argument)
    {
        shared_state &state = *static_cast<shared_state *>(argument);

        wait_for_readers(state);

        for (uint32_t i = 0; i < WRITES; i++)
        {
            state.table_.increment(ALL_COUNTERS[i % ALL_COUNTERS.size()], i % 7);
        }

        state.writer_done_.store(1, minstd::memory_order_release);
        return nullptr;
    }

    //  A reader sees a single value never go backwards, nor past what the writer will write.
    void *read_one_entry_monotonically(void *argument)
    {
        shared_state &state = *static_cast<shared_state *>(argument);
        uint64_t previous = 0;

        state.readers_started_.fetch_add(1, minstd::memory_order_acq_rel);

        do
        {
            const uint64_t current = state.table_.value(test_counter::FRAMES);

            if ((current < previous) || (current > WRITES))
            {
                state.failures_.fetch_add(1, minstd::memory_order_relaxed);
            }

            previous = current;
        } while (state.writer_done_.load(minstd::memory_order_acquire) == 0);

        return nullptr;
    }

    void *read_for_tears(void *argument)
    {
        shared_state &state = *static_cast<shared_state *>(argument);

        state.readers_started_.fetch_add(1, minstd::memory_order_acq_rel);

        do
        {
            const uint64_t current = state.table_.value(test_counter::ERRORS);

            if ((current >> 32) != (current & 0xFFFFFFFFULL))
            {
                state.failures_.fetch_add(1, minstd::memory_order_relaxed);
            }
        } while (state.writer_done_.load(minstd::memory_order_acquire) == 0);

        return nullptr;
    }

    //  for_each during writes: each entry, on its own, never goes backwards.
    void *iterate_monotonically(void *argument)
    {
        shared_state &state = *static_cast<shared_state *>(argument);
        minstd::array<uint64_t, test_table::SIZE> previous{};

        state.readers_started_.fetch_add(1, minstd::memory_order_acq_rel);

        do
        {
            uint32_t visits = 0;

            state.table_.for_each([&](const test_table::descriptor_type &descriptor, uint64_t value) {
                const size_t index = static_cast<size_t>(descriptor.which_);

                if ((index != visits) || (value < previous[index]))
                {
                    state.failures_.fetch_add(1, minstd::memory_order_relaxed);
                }

                previous[index] = value;
                visits++;
            });

            if (visits != test_table::SIZE)
            {
                state.failures_.fetch_add(1, minstd::memory_order_relaxed);
            }
        } while (state.writer_done_.load(minstd::memory_order_acquire) == 0);

        return nullptr;
    }

    void run_writer_and_readers(shared_state &state, void *(*writer)(void *), void *(*reader)(void *))
    {
        pthread_t writer_thread;
        minstd::array<pthread_t, READERS> reader_threads;

        for (pthread_t &thread : reader_threads)
        {
            CHECK_EQUAL(0, pthread_create(&thread, nullptr, reader, &state));
        }

        CHECK_EQUAL(0, pthread_create(&writer_thread, nullptr, writer, &state));

        CHECK_EQUAL(0, pthread_join(writer_thread, nullptr));

        for (pthread_t &thread : reader_threads)
        {
            CHECK_EQUAL(0, pthread_join(thread, nullptr));
        }
    }

    TEST(StatisticsTableConcurrencyTests, ReadersNeverSeeAValueGoBackwards)
    {
        shared_state state;

        run_writer_and_readers(state, write_one_entry, read_one_entry_monotonically);

        CHECK_EQUAL(0, state.failures_.load());
        CHECK_EQUAL(WRITES, state.table_.value(test_counter::FRAMES));
    }

    TEST(StatisticsTableConcurrencyTests, ReadersNeverSeeATornValue)
    {
        shared_state state;

        run_writer_and_readers(state, write_tear_detecting_steps, read_for_tears);

        CHECK_EQUAL(0, state.failures_.load());
        CHECK_EQUAL(TEAR_DETECTING_STEP * WRITES, state.table_.value(test_counter::ERRORS));
    }

    TEST(StatisticsTableConcurrencyTests, ForEachDuringWritesSeesEveryEntryInOrderAndNeverBackwards)
    {
        shared_state state;

        run_writer_and_readers(state, write_every_entry_in_turn, iterate_monotonically);

        CHECK_EQUAL(0, state.failures_.load());

        minstd::array<uint64_t, test_table::SIZE> expected{};

        for (uint32_t i = 0; i < WRITES; i++)
        {
            expected[i % ALL_COUNTERS.size()] += i % 7;
        }

        for (test_counter which : ALL_COUNTERS)
        {
            CHECK_EQUAL(expected[static_cast<size_t>(which)], state.table_.value(which));
        }
    }

    //  Several tables, each with its own writer, read together by other threads.

    constexpr uint32_t TABLES = 4;

    struct multi_table_state
    {
        minstd::array<test_table, TABLES>       tables_;
        minstd::atomic<uint32_t>                writers_done_{0};
        minstd::atomic<uint32_t>                failures_{0};
    };

    struct writer_argument
    {
        multi_table_state       *state_;
        uint32_t                 table_;
    };

    void *write_own_table(void *argument)
    {
        const writer_argument &own = *static_cast<writer_argument *>(argument);
        test_table &table = own.state_->tables_[own.table_];

        for (uint32_t i = 0; i < WRITES; i++)
        {
            table.increment(test_counter::FRAMES);
            table.increment(test_counter::DROPS, own.table_ + 1);
        }

        own.state_->writers_done_.fetch_add(1, minstd::memory_order_acq_rel);
        return nullptr;
    }

    void *read_every_table(void *argument)
    {
        multi_table_state &state = *static_cast<multi_table_state *>(argument);
        minstd::array<uint64_t, TABLES> previous{};

        do
        {
            for (uint32_t t = 0; t < TABLES; t++)
            {
                const uint64_t frames = state.tables_[t].value(test_counter::FRAMES);

                if ((frames < previous[t]) || (frames > WRITES) || (state.tables_[t].value(test_counter::ERRORS) != 0))
                {
                    state.failures_.fetch_add(1, minstd::memory_order_relaxed);
                }

                previous[t] = frames;
            }
        } while (state.writers_done_.load(minstd::memory_order_acquire) < TABLES);

        return nullptr;
    }

    TEST(StatisticsTableConcurrencyTests, TablesWithTheirOwnWritersStayIndependent)
    {
        multi_table_state state;
        minstd::array<writer_argument, TABLES> arguments;
        minstd::array<pthread_t, TABLES> writer_threads;
        minstd::array<pthread_t, READERS> reader_threads;

        for (pthread_t &thread : reader_threads)
        {
            CHECK_EQUAL(0, pthread_create(&thread, nullptr, read_every_table, &state));
        }

        for (uint32_t t = 0; t < TABLES; t++)
        {
            arguments[t] = writer_argument{.state_ = &state, .table_ = t};
            CHECK_EQUAL(0, pthread_create(&writer_threads[t], nullptr, write_own_table, &arguments[t]));
        }

        for (pthread_t &thread : writer_threads)
        {
            CHECK_EQUAL(0, pthread_join(thread, nullptr));
        }

        for (pthread_t &thread : reader_threads)
        {
            CHECK_EQUAL(0, pthread_join(thread, nullptr));
        }

        CHECK_EQUAL(0, state.failures_.load());

        for (uint32_t t = 0; t < TABLES; t++)
        {
            CHECK_EQUAL(WRITES, state.tables_[t].value(test_counter::FRAMES));
            CHECK_EQUAL(0, state.tables_[t].value(test_counter::ERRORS));
            CHECK_EQUAL(static_cast<uint64_t>(WRITES) * (t + 1), state.tables_[t].value(test_counter::DROPS));
        }
    }
}
