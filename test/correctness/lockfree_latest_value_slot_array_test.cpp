// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <lockfree/latest_value_slot_array>

#include <buffer>
#include <fixed_string>

#include <pthread.h>
#include <stdint.h>
#include <string.h>

namespace
{
    using minstd::lockfree::latest_value_result;

    //  Compare results as ints so a failing check prints the values.

#define CHECK_RESULT(expected, expression) \
    CHECK_EQUAL(static_cast<int>(latest_value_result::expected), static_cast<int>(expression))

    //  successful stores across all writers of a multi-threaded test.

    template <typename WriterArgs>
    uint64_t total_wins(const WriterArgs *args, size_t count)
    {
        uint64_t total = 0;

        for (size_t i = 0; i < count; ++i)
        {
            total += args[i].wins.load();
        }

        return total;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(LatestValueSlotArrayTests)
    {
    };
#pragma GCC diagnostic pop

    //  -----------------------------------------------------------------------
    //  Constants / defaults
    //  -----------------------------------------------------------------------

    TEST(LatestValueSlotArrayTests, ZeroValueAfterConstruction)
    {
        minstd::lockfree::latest_value_slot_array<uint32_t, 4> slot;

        CHECK_EQUAL(0u, slot.load());
        CHECK_FALSE(slot.has_value());
    }

    //  -----------------------------------------------------------------------
    //  Round trip and wrap-around
    //  -----------------------------------------------------------------------

    TEST(LatestValueSlotArrayTests, RoundTrip)
    {
        minstd::lockfree::latest_value_slot_array<uint64_t, 4> slot;

        CHECK_RESULT(SUCCESS, slot.store(0x1122334455667788ull));
        CHECK_EQUAL(0x1122334455667788ull, slot.load());

        CHECK_RESULT(SUCCESS, slot.store(0xAABBCCDDEEFF0011ull));
        CHECK_EQUAL(0xAABBCCDDEEFF0011ull, slot.load());
    }

    TEST(LatestValueSlotArrayTests, WrapAroundOverManyStores)
    {
        static constexpr size_t SLOTS = 3;
        minstd::lockfree::latest_value_slot_array<uint32_t, SLOTS> slot;

        //  More stores than slots forces the index to wrap many times.

        const uint32_t total = static_cast<uint32_t>(SLOTS) * 7 + 1;

        for (uint32_t i = 0; i < total; ++i)
        {
            CHECK_RESULT(SUCCESS, slot.store(i));
        }

        CHECK_EQUAL(total - 1, slot.load());
    }

    //  -----------------------------------------------------------------------
    //  Large T and non-multiple-of-8 T
    //  -----------------------------------------------------------------------

    struct LargePayload
    {
        uint64_t header;
        uint8_t  bytes[248];
    };

    static_assert(sizeof(LargePayload) == 256, "LargePayload must be 256 bytes");

    TEST(LatestValueSlotArrayTests, LargeT)
    {
        minstd::lockfree::latest_value_slot_array<LargePayload, 4> slot;

        LargePayload value{};
        value.header = 0xDEADBEEFCAFEBABEull;

        for (size_t i = 0; i < sizeof(value.bytes); ++i)
        {
            value.bytes[i] = static_cast<uint8_t>(i);
        }

        CHECK_RESULT(SUCCESS, slot.store(value));

        const LargePayload out = slot.load();
        CHECK_EQUAL(value.header, out.header);
        CHECK_EQUAL(0, memcmp(value.bytes, out.bytes, sizeof(value.bytes)));
    }

    TEST(LatestValueSlotArrayTests, NonMultipleOfEightT)
    {
        //  A 5-byte payload: not a multiple of 8.

        struct FiveBytes
        {
            uint8_t a, b, c, d, e;
        };

        static_assert(sizeof(FiveBytes) == 5, "FiveBytes must be 5 bytes");
        minstd::lockfree::latest_value_slot_array<FiveBytes, 4> small;

        FiveBytes value{};
        value.a = 1;
        value.b = 2;
        value.c = 3;
        value.d = 4;
        value.e = 5;

        CHECK_RESULT(SUCCESS, small.store(value));
        const FiveBytes out = small.load();
        CHECK_EQUAL(value.a, out.a);
        CHECK_EQUAL(value.b, out.b);
        CHECK_EQUAL(value.c, out.c);
        CHECK_EQUAL(value.d, out.d);
        CHECK_EQUAL(value.e, out.e);
    }

    //  -----------------------------------------------------------------------
    //  Multi-threaded: 4 writers x 4 readers, self-validating payloads.
    //
    //  Every payload is a block of identical bytes plus a length field, so a
    //  reader can detect any torn read.  Some store must succeed; when
    //  quiesced the final load() is a consistent value.
    //  -----------------------------------------------------------------------

    static constexpr size_t MT_WRITERS      = 4;
    static constexpr size_t MT_READERS      = 4;
    static constexpr size_t MT_ITERATIONS   = 20000;

    struct mt_payload
    {
        uint32_t length;      //  number of data bytes
        uint32_t writer;      //  which writer produced it
        uint8_t  data[64];    //  all bytes equal to `length`
    };

    static bool payload_is_consistent(const mt_payload &p) noexcept
    {
        if (p.length > sizeof(p.data))
        {
            return false;
        }

        for (uint32_t i = 0; i < p.length; ++i)
        {
            if (p.data[i] != static_cast<uint8_t>(p.length))
            {
                return false;
            }
        }

        return true;
    }

    struct writer_args
    {
        minstd::lockfree::latest_value_slot_array<mt_payload, 8> *slot;
        uint32_t                                                  id;
        minstd::atomic<bool>                                      stop{false};
        minstd::atomic<uint32_t>                                  wins{0};
        minstd::atomic<uint32_t>                                  unexpected{0};
    };

    void *writer_thread(void *arg)
    {
        writer_args *a = static_cast<writer_args *>(arg);
        mt_payload value{};
        value.writer = a->id;

        for (uint32_t i = 1; i <= MT_ITERATIONS; ++i)
        {
            value.length = static_cast<uint32_t>(i % sizeof(value.data)) + 1;

            for (uint32_t b = 0; b < value.length; ++b)
            {
                value.data[b] = static_cast<uint8_t>(value.length);
            }

            const latest_value_result result = a->slot->store(value);

            if (result == latest_value_result::SUCCESS)
            {
                a->wins.fetch_add(1, minstd::memory_order_relaxed);
            }
            else if (result != latest_value_result::FAILURE)
            {
                a->unexpected.fetch_add(1, minstd::memory_order_relaxed);    //  a T never truncates
            }
        }

        return nullptr;
    }

    struct reader_args
    {
        minstd::lockfree::latest_value_slot_array<mt_payload, 8> *slot;
        minstd::atomic<bool>      stop{false};
        minstd::atomic<uint64_t> reads{0};
        minstd::atomic<uint64_t> torn{0};
    };

    void *reader_thread(void *arg)
    {
        reader_args *a = static_cast<reader_args *>(arg);

        while (!a->stop.load(minstd::memory_order_acquire))
        {
            const mt_payload p = a->slot->load();
            a->reads.fetch_add(1, minstd::memory_order_relaxed);

            if (!payload_is_consistent(p))
            {
                a->torn.fetch_add(1, minstd::memory_order_relaxed);
            }
        }

        return nullptr;
    }

    TEST(LatestValueSlotArrayTests, MultiThreadedWritersAndReaders)
    {
        minstd::lockfree::latest_value_slot_array<mt_payload, 8> slot;

        reader_args rargs;
        rargs.slot = &slot;

        pthread_t readers[MT_READERS];

        for (size_t i = 0; i < MT_READERS; ++i)
        {
            pthread_create(&readers[i], nullptr, reader_thread, &rargs);
        }

        writer_args wargs[MT_WRITERS];
        pthread_t writers[MT_WRITERS];

        for (size_t i = 0; i < MT_WRITERS; ++i)
        {
            wargs[i].slot = &slot;
            wargs[i].id   = static_cast<uint32_t>(i);
            pthread_create(&writers[i], nullptr, writer_thread, &wargs[i]);
        }

        for (size_t i = 0; i < MT_WRITERS; ++i)
        {
            pthread_join(writers[i], nullptr);
        }

        for (size_t i = 0; i < MT_READERS; ++i)
        {
            rargs.stop.store(true, minstd::memory_order_release);
        }

        for (size_t i = 0; i < MT_READERS; ++i)
        {
            pthread_join(readers[i], nullptr);
        }

        //  No reader ever observed a torn value.
        CHECK_EQUAL(0ull, rargs.torn.load());

        //  Some store succeeded.  Not every writer need win one: store() drops whenever another store is in
        //      flight, and the writer that just released writer_busy_ usually re-takes it before a competing
        //      core sees it free, so one writer can lose every attempt (lock-free, not starvation-free).

        CHECK_TRUE(total_wins(wargs, MT_WRITERS) >= 1);

        for (size_t i = 0; i < MT_WRITERS; ++i)
        {
            CHECK_EQUAL(0u, wargs[i].unexpected.load());   //  every store was SUCCESS or FAILURE
        }

        //  Quiesced: the final load() equals the last successful store.
        const mt_payload final_value = slot.load();
        CHECK(payload_is_consistent(final_value));
    }

    //  -----------------------------------------------------------------------
    //  Multi-threaded: one writer storing 1, 2, 3, ...; each reader's
    //  successive loads must never go backwards, and a load must return
    //  EMPTY exactly when it sees the initial 0 (the readers start before the
    //  writer, so some loads see nothing stored yet).
    //
    //  A load that validated a lapped slot against "even and unchanged"
    //  alone could return a value the writer had written but not yet
    //  published, and the reader's next load the older published one.  Two
    //  slots make laps as likely as possible.  The interleaving is rare, so
    //  this is a regression net; LatestValueSlotArray_bug_unpublished_read.cfg
    //  is the exhaustive check.
    //  -----------------------------------------------------------------------

    static constexpr uint64_t MONOTONIC_STORES  = 200000;
    static constexpr size_t   MONOTONIC_READERS = 4;

    struct monotonic_args
    {
        minstd::lockfree::latest_value_slot_array<uint64_t, 2> *slot;
        minstd::atomic<bool>                                    stop{false};
        minstd::atomic<uint64_t>                                regressions{0};
        minstd::atomic<uint64_t>                                mislabelled{0};
    };

    void *monotonic_writer_thread(void *arg)
    {
        monotonic_args *a = static_cast<monotonic_args *>(arg);

        for (uint64_t v = 1; v <= MONOTONIC_STORES; ++v)
        {
            a->slot->store(v);      //  sole writer: always SUCCESS
        }

        a->stop.store(true, minstd::memory_order_release);

        return nullptr;
    }

    void *monotonic_reader_thread(void *arg)
    {
        monotonic_args *a = static_cast<monotonic_args *>(arg);
        uint64_t previous = 0;

        while (!a->stop.load(minstd::memory_order_acquire))
        {
            uint64_t v = 0;
            const latest_value_result result = a->slot->load(v);

            if (v < previous)
            {
                a->regressions.fetch_add(1, minstd::memory_order_relaxed);
            }

            if ((result == latest_value_result::EMPTY) != (v == 0))
            {
                a->mislabelled.fetch_add(1, minstd::memory_order_relaxed);
            }

            previous = v;
        }

        return nullptr;
    }

    TEST(LatestValueSlotArrayTests, SingleWriterLoadsNeverGoBackwards)
    {
        minstd::lockfree::latest_value_slot_array<uint64_t, 2> slot;

        monotonic_args args;
        args.slot = &slot;

        pthread_t readers[MONOTONIC_READERS];

        for (size_t i = 0; i < MONOTONIC_READERS; ++i)
        {
            pthread_create(&readers[i], nullptr, monotonic_reader_thread, &args);
        }

        pthread_t writer;
        pthread_create(&writer, nullptr, monotonic_writer_thread, &args);

        pthread_join(writer, nullptr);

        for (size_t i = 0; i < MONOTONIC_READERS; ++i)
        {
            pthread_join(readers[i], nullptr);
        }

        CHECK_EQUAL(0ull, args.regressions.load());
        CHECK_EQUAL(0ull, args.mislabelled.load());
        CHECK_EQUAL(MONOTONIC_STORES, slot.load());
    }

    //  -----------------------------------------------------------------------
    //  latest_value_text  (single-threaded)
    //  -----------------------------------------------------------------------

    TEST(LatestValueSlotArrayTests, TextRoundTrip)
    {
        minstd::lockfree::latest_value_text<char, 16> text;

        CHECK_EQUAL(0u, text.length());

        CHECK_RESULT(SUCCESS, text.store("hello"));
        CHECK_EQUAL(5u, text.length());

        char buf[16];
        CHECK_RESULT(SUCCESS, text.load(buf, sizeof(buf)));
        STRCMP_EQUAL("hello", buf);
    }

    TEST(LatestValueSlotArrayTests, TextTruncatesToMaxLen)
    {
        minstd::lockfree::latest_value_text<char, 4> text;

        CHECK_RESULT(TRUNCATED, text.store("abcdefghij"));   //  longer than MaxLen = 4
        CHECK_EQUAL(4u, text.length());

        char buf[8] = {0};
        CHECK_RESULT(SUCCESS, text.load(buf, sizeof(buf)));
        STRCMP_EQUAL("abcd", buf);
    }

    TEST(LatestValueSlotArrayTests, TextEmptyString)
    {
        minstd::lockfree::latest_value_text<char, 8> text;

        CHECK_RESULT(SUCCESS, text.store(""));
        CHECK_EQUAL(0u, text.length());

        char buf[8] = {'x', 'y', 'z'};
        CHECK_RESULT(SUCCESS, text.load(buf, sizeof(buf)));
        STRCMP_EQUAL("", buf);
    }

    TEST(LatestValueSlotArrayTests, TextLoadIntoUndersizedBuffer)
    {
        minstd::lockfree::latest_value_text<char, 16> text;
        CHECK_RESULT(SUCCESS, text.store("hello world"));

        char buf[4];
        CHECK_RESULT(TRUNCATED, text.load(buf, sizeof(buf)));      //  room for 3 chars + NUL
        STRCMP_EQUAL("hel", buf);
    }

    TEST(LatestValueSlotArrayTests, TextStoreAndLoadBasicString)
    {
        minstd::lockfree::latest_value_text<char, 16> text;

        minstd::fixed_string<32> src = "hello";
        CHECK_RESULT(SUCCESS, text.store(src));
        CHECK_EQUAL(5u, text.length());

        minstd::fixed_string<32> out;
        CHECK_RESULT(SUCCESS, text.load(out));
        STRCMP_EQUAL("hello", out.c_str());
        CHECK_EQUAL(5u, out.length());
    }

    TEST(LatestValueSlotArrayTests, TextLoadBasicStringClipsToCapacity)
    {
        minstd::lockfree::latest_value_text<char, 16> text;
        CHECK_RESULT(SUCCESS, text.store("0123456789"));   //  10 chars

        minstd::fixed_string<4> out;       //  holds at most 4 chars
        CHECK_RESULT(TRUNCATED, text.load(out));
        STRCMP_EQUAL("0123", out.c_str());
        CHECK_EQUAL(4u, out.length());
    }

    //  -----------------------------------------------------------------------
    //  latest_value_blob  (single-threaded)
    //  -----------------------------------------------------------------------

    TEST(LatestValueSlotArrayTests, BlobRoundTrip)
    {
        minstd::lockfree::latest_value_blob<16> blob;

        const uint8_t src[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        CHECK_RESULT(SUCCESS, blob.store(src, 16));
        CHECK_EQUAL(16u, blob.size());

        uint8_t dst[16] = {0};
        CHECK_RESULT(SUCCESS, blob.load(dst, 16));
        CHECK_EQUAL(0, memcmp(src, dst, 16));
    }

    TEST(LatestValueSlotArrayTests, BlobPartialLength)
    {
        minstd::lockfree::latest_value_blob<16> blob;

        const uint8_t src[8] = {9, 8, 7, 6, 5, 4, 3, 2};
        CHECK_RESULT(SUCCESS, blob.store(src, 8));
        CHECK_EQUAL(8u, blob.size());

        uint8_t dst[16] = {0};
        CHECK_RESULT(SUCCESS, blob.load(dst, 16));
        CHECK_EQUAL(0, memcmp(src, dst, 8));
        CHECK_EQUAL(0u, dst[8]);   //  remainder zero-filled
    }

    TEST(LatestValueSlotArrayTests, BlobStoreTruncatesToMaxBytes)
    {
        minstd::lockfree::latest_value_blob<4> blob;

        const uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        CHECK_RESULT(TRUNCATED, blob.store(src, 8));
        CHECK_EQUAL(4u, blob.size());

        uint8_t dst[8] = {0};
        CHECK_RESULT(SUCCESS, blob.load(dst, 8));
        CHECK_EQUAL(1u, dst[0]);
        CHECK_EQUAL(4u, dst[3]);
    }

    TEST(LatestValueSlotArrayTests, BlobLoadPartial)
    {
        minstd::lockfree::latest_value_blob<16> blob;
        const uint8_t src[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        CHECK_RESULT(SUCCESS, blob.store(src, 16));

        uint8_t dst[4];
        CHECK_RESULT(TRUNCATED, blob.load(dst, 4));
        CHECK_EQUAL(1u, dst[0]);
        CHECK_EQUAL(4u, dst[3]);
    }

    TEST(LatestValueSlotArrayTests, BlobStoreAndLoadBuffer)
    {
        minstd::lockfree::latest_value_blob<16> blob;

        minstd::stack_buffer<uint8_t, 16> src;
        const uint8_t values[5] = {1, 2, 3, 4, 5};
        CHECK_EQUAL(5u, src.append(values, 5));
        CHECK_RESULT(SUCCESS, blob.store(src));
        CHECK_EQUAL(5u, blob.size());

        minstd::stack_buffer<uint8_t, 16> out;
        CHECK_RESULT(SUCCESS, blob.load(out));
        CHECK_EQUAL(5u, out.size());

        for (size_t i = 0; i < 5; ++i)
        {
            CHECK_EQUAL(values[i], out[i]);
        }
    }

    TEST(LatestValueSlotArrayTests, BlobLoadBufferClipsToCapacity)
    {
        minstd::lockfree::latest_value_blob<16> blob;
        const uint8_t values[8] = {9, 8, 7, 6, 5, 4, 3, 2};
        CHECK_RESULT(SUCCESS, blob.store(values, 8));

        minstd::stack_buffer<uint8_t, 4> out;   //  holds at most 4 bytes
        CHECK_RESULT(TRUNCATED, blob.load(out));
        CHECK_EQUAL(4u, out.size());
        CHECK_EQUAL(9u, out[0]);
        CHECK_EQUAL(6u, out[3]);
    }

    //  -----------------------------------------------------------------------
    //  Results: EMPTY before the first store, and SUCCESS / TRUNCATED
    //  boundaries (single-threaded, so a store never sees another in flight
    //  and never fails)
    //  -----------------------------------------------------------------------

    TEST(LatestValueSlotArrayTests, LoadIsEmptyUntilTheFirstStore)
    {
        minstd::lockfree::latest_value_slot_array<uint32_t, 2> slot;
        uint32_t out = 99;

        CHECK_FALSE(slot.has_value());
        CHECK_RESULT(EMPTY, slot.load(out));
        CHECK_EQUAL(0u, out);

        //  A stored zero is the same value as the initial one, but is not empty.

        CHECK_RESULT(SUCCESS, slot.store(0u));
        CHECK_TRUE(slot.has_value());
        out = 99;
        CHECK_RESULT(SUCCESS, slot.load(out));
        CHECK_EQUAL(0u, out);

        CHECK_RESULT(SUCCESS, slot.store(7u));
        CHECK_RESULT(SUCCESS, slot.load(out));
        CHECK_EQUAL(7u, out);
    }

    TEST(LatestValueSlotArrayTests, HasValueSurvivesSlotIndexWrapAround)
    {
        minstd::lockfree::latest_value_slot_array<uint32_t, 3> slot;
        uint32_t out = 0;

        for (uint32_t i = 1; i <= 3 * 5 + 1; ++i)
        {
            CHECK_RESULT(SUCCESS, slot.store(i));
            CHECK_TRUE(slot.has_value());
            CHECK_RESULT(SUCCESS, slot.load(out));
            CHECK_EQUAL(i, out);
        }
    }

    TEST(LatestValueSlotArrayTests, TextIsEmptyUntilTheFirstStore)
    {
        minstd::lockfree::latest_value_text<char, 8> text;

        CHECK_FALSE(text.has_value());
        CHECK_EQUAL(0u, text.length());

        char buf[4] = {'x', 'x', 'x', 'x'};
        CHECK_RESULT(EMPTY, text.load(buf, sizeof(buf)));
        STRCMP_EQUAL("", buf);

        char untouched = 'x';
        CHECK_RESULT(EMPTY, text.load(&untouched, 0));
        CHECK_EQUAL('x', untouched);

        minstd::fixed_string<4> out("abc");
        CHECK_RESULT(EMPTY, text.load(out));
        CHECK_EQUAL(0u, out.length());

        //  A stored empty string is not empty.

        CHECK_RESULT(SUCCESS, text.store(""));
        CHECK_TRUE(text.has_value());
        CHECK_RESULT(SUCCESS, text.load(buf, sizeof(buf)));
        CHECK_RESULT(SUCCESS, text.load(&untouched, 0));
        CHECK_RESULT(SUCCESS, text.load(out));
    }

    TEST(LatestValueSlotArrayTests, BlobIsEmptyUntilTheFirstStore)
    {
        minstd::lockfree::latest_value_blob<8> blob;

        CHECK_FALSE(blob.has_value());
        CHECK_EQUAL(0u, blob.size());

        uint8_t dst[4] = {9, 9, 9, 9};
        CHECK_RESULT(EMPTY, blob.load(dst, sizeof(dst)));
        CHECK_EQUAL(9, dst[0]);                                 //  nothing copied
        CHECK_RESULT(EMPTY, blob.load(nullptr, 0));

        minstd::stack_buffer<uint8_t, 4> out;
        out.append(static_cast<uint8_t>(1));
        CHECK_RESULT(EMPTY, blob.load(out));
        CHECK_EQUAL(0u, out.size());

        //  A stored zero-length blob is not empty.

        CHECK_RESULT(SUCCESS, blob.store(nullptr, 0));
        CHECK_TRUE(blob.has_value());
        CHECK_RESULT(SUCCESS, blob.load(dst, sizeof(dst)));
        CHECK_RESULT(SUCCESS, blob.load(nullptr, 0));
        CHECK_RESULT(SUCCESS, blob.load(out));
    }

    TEST(LatestValueSlotArrayTests, TextStoreResultsAtTheMaxLenBoundary)
    {
        minstd::lockfree::latest_value_text<char, 4> text;

        CHECK_RESULT(SUCCESS, text.store("abc"));               //  under MaxLen
        CHECK_RESULT(SUCCESS, text.store("abcd"));              //  exactly MaxLen
        CHECK_EQUAL(4u, text.length());
        CHECK_RESULT(TRUNCATED, text.store("abcde"));           //  one over
        CHECK_EQUAL(4u, text.length());

        CHECK_RESULT(SUCCESS, text.store("abcdef", 4));         //  explicit length, exactly MaxLen
        CHECK_RESULT(TRUNCATED, text.store("abcdef", 5));       //  explicit length, one over
        CHECK_RESULT(SUCCESS, text.store("ab\0z", 4));          //  explicit length may carry a NUL ...
        CHECK_EQUAL(2u, text.length());                         //  ... which ends the loaded string

        minstd::fixed_string<16> exact = "wxyz";
        minstd::fixed_string<16> longer = "vwxyz";
        CHECK_RESULT(SUCCESS, text.store(exact));
        CHECK_RESULT(TRUNCATED, text.store(longer));

        char buf[8];
        CHECK_RESULT(SUCCESS, text.load(buf, sizeof(buf)));
        STRCMP_EQUAL("vwxy", buf);
    }

    TEST(LatestValueSlotArrayTests, TextStoreNullptrStoresTheEmptyString)
    {
        minstd::lockfree::latest_value_text<char, 8> text;
        CHECK_RESULT(SUCCESS, text.store("abc"));

        CHECK_RESULT(SUCCESS, text.store(static_cast<const char *>(nullptr)));
        CHECK_EQUAL(0u, text.length());
    }

    TEST(LatestValueSlotArrayTests, TextLoadResultsAtTheBufferBoundary)
    {
        minstd::lockfree::latest_value_text<char, 16> text;
        CHECK_RESULT(SUCCESS, text.store("hello"));

        char exact[6];                                          //  5 chars + NUL
        CHECK_RESULT(SUCCESS, text.load(exact, sizeof(exact)));
        STRCMP_EQUAL("hello", exact);

        char short_by_one[5];
        CHECK_RESULT(TRUNCATED, text.load(short_by_one, sizeof(short_by_one)));
        STRCMP_EQUAL("hell", short_by_one);

        char untouched = 'x';
        CHECK_RESULT(TRUNCATED, text.load(&untouched, 0));      //  nothing fits, nothing written
        CHECK_EQUAL('x', untouched);

        CHECK_RESULT(SUCCESS, text.store(""));
        CHECK_RESULT(SUCCESS, text.load(&untouched, 0));        //  the empty string fits in nothing
        CHECK_EQUAL('x', untouched);

        CHECK_RESULT(SUCCESS, text.store("abcd"));
        minstd::fixed_string<4> fits;
        CHECK_RESULT(SUCCESS, text.load(fits));
        STRCMP_EQUAL("abcd", fits.c_str());
    }

    TEST(LatestValueSlotArrayTests, BlobStoreResultsAtTheMaxBytesBoundary)
    {
        minstd::lockfree::latest_value_blob<4> blob;
        const uint8_t src[6] = {1, 2, 3, 4, 5, 6};

        CHECK_RESULT(SUCCESS, blob.store(src, 0));
        CHECK_EQUAL(0u, blob.size());
        CHECK_RESULT(SUCCESS, blob.store(src, 3));
        CHECK_RESULT(SUCCESS, blob.store(src, 4));              //  exactly MaxBytes
        CHECK_RESULT(TRUNCATED, blob.store(src, 5));            //  one over
        CHECK_EQUAL(4u, blob.size());

        minstd::stack_buffer<uint8_t, 8> exact;
        exact.append(src, 4);
        minstd::stack_buffer<uint8_t, 8> longer;
        longer.append(src, 6);
        CHECK_RESULT(SUCCESS, blob.store(exact));
        CHECK_RESULT(TRUNCATED, blob.store(longer));
        CHECK_EQUAL(4u, blob.size());
    }

    TEST(LatestValueSlotArrayTests, BlobLoadResultsAtTheBufferBoundary)
    {
        minstd::lockfree::latest_value_blob<16> blob;
        const uint8_t src[5] = {1, 2, 3, 4, 5};
        CHECK_RESULT(SUCCESS, blob.store(src, 5));

        uint8_t exact[5] = {};
        CHECK_RESULT(SUCCESS, blob.load(exact, 5));
        CHECK_EQUAL(0, memcmp(src, exact, 5));

        uint8_t short_by_one[4] = {};
        CHECK_RESULT(TRUNCATED, blob.load(short_by_one, 4));
        CHECK_EQUAL(0, memcmp(src, short_by_one, 4));

        CHECK_RESULT(TRUNCATED, blob.load(nullptr, 0));         //  nothing fits, nothing written

        minstd::stack_buffer<uint8_t, 5> fits;
        CHECK_RESULT(SUCCESS, blob.load(fits));
        CHECK_EQUAL(5u, fits.size());

        CHECK_RESULT(SUCCESS, blob.store(src, 0));
        CHECK_RESULT(SUCCESS, blob.load(nullptr, 0));           //  an empty blob fits in nothing
    }

    //  -----------------------------------------------------------------------
    //  latest_value_text  (multi-threaded)
    //
    //  Every stored string is a run of one identical byte (never NUL), so a
    //  torn read -- which the seqlock must prevent -- would surface as two
    //  differing bytes in the snapshot.
    //  -----------------------------------------------------------------------

    static constexpr size_t TEXT_MAXLEN = 32;
    static constexpr size_t TEXT_WRITERS = 4;
    static constexpr size_t TEXT_READERS = 4;
    static constexpr size_t TEXT_ITERS   = 20000;

    struct text_writer_args
    {
        minstd::lockfree::latest_value_text<char, TEXT_MAXLEN> *text;
        uint32_t                                               id;
        minstd::atomic<uint32_t>                               wins{0};
    };

    void *text_writer_thread(void *arg)
    {
        text_writer_args *a = static_cast<text_writer_args *>(arg);
        char buf[TEXT_MAXLEN + 1];

        for (uint32_t i = 1; i <= TEXT_ITERS; ++i)
        {
            const char  c   = static_cast<char>(((a->id * 7u + i) % 127u) + 1u);
            const size_t len = (i % TEXT_MAXLEN) + 1;

            for (size_t k = 0; k < len; ++k)
            {
                buf[k] = c;
            }

            buf[len] = 0;

            if (a->text->store(buf) == latest_value_result::SUCCESS)
            {
                a->wins.fetch_add(1, minstd::memory_order_relaxed);
            }
        }

        return nullptr;
    }

    struct text_reader_args
    {
        minstd::lockfree::latest_value_text<char, TEXT_MAXLEN> *text;
        minstd::atomic<bool>      stop{false};
        minstd::atomic<uint64_t> reads{0};
        minstd::atomic<uint64_t> torn{0};
    };

    static bool text_is_consistent(const char *s) noexcept
    {
        if (s[0] == '\0')
        {
            return true;
        }

        const char c = s[0];

        for (size_t i = 1; s[i] != '\0'; ++i)
        {
            if (s[i] != c)
            {
                return false;
            }
        }

        return true;
    }

    void *text_reader_thread(void *arg)
    {
        text_reader_args *a = static_cast<text_reader_args *>(arg);
        char buf[TEXT_MAXLEN + 1];

        while (!a->stop.load(minstd::memory_order_acquire))
        {
            a->text->load(buf, sizeof(buf));
            a->reads.fetch_add(1, minstd::memory_order_relaxed);

            if (!text_is_consistent(buf))
            {
                a->torn.fetch_add(1, minstd::memory_order_relaxed);
            }
        }

        return nullptr;
    }

    TEST(LatestValueSlotArrayTests, TextMultiThreadedWritersAndReaders)
    {
        minstd::lockfree::latest_value_text<char, TEXT_MAXLEN> text;

        text_reader_args rargs;
        rargs.text = &text;
        pthread_t readers[TEXT_READERS];

        for (size_t i = 0; i < TEXT_READERS; ++i)
        {
            pthread_create(&readers[i], nullptr, text_reader_thread, &rargs);
        }

        text_writer_args wargs[TEXT_WRITERS];
        pthread_t writers[TEXT_WRITERS];

        for (size_t i = 0; i < TEXT_WRITERS; ++i)
        {
            wargs[i].text = &text;
            wargs[i].id   = static_cast<uint32_t>(i);
            pthread_create(&writers[i], nullptr, text_writer_thread, &wargs[i]);
        }

        for (size_t i = 0; i < TEXT_WRITERS; ++i)
        {
            pthread_join(writers[i], nullptr);
        }

        for (size_t i = 0; i < TEXT_READERS; ++i)
        {
            rargs.stop.store(true, minstd::memory_order_release);
        }

        for (size_t i = 0; i < TEXT_READERS; ++i)
        {
            pthread_join(readers[i], nullptr);
        }

        CHECK_EQUAL(0ull, rargs.torn.load());
        CHECK_TRUE(total_wins(wargs, TEXT_WRITERS) >= 1);       //  see MultiThreadedWritersAndReaders

        char buf[TEXT_MAXLEN + 1];
        text.load(buf, sizeof(buf));
        CHECK_TRUE(text_is_consistent(buf));
    }

    //  -----------------------------------------------------------------------
    //  latest_value_blob  (multi-threaded)
    //
    //  Each stored blob is a run of one identical non-zero byte followed by
    //  zeros.  A torn read would break that shape, so validating the whole
    //  snapshot (data region uniform, tail zero) proves no tearing occurred.
    //  -----------------------------------------------------------------------

    static constexpr size_t BLOB_MAXBYTES = 64;
    static constexpr size_t BLOB_WRITERS  = 4;
    static constexpr size_t BLOB_READERS  = 4;
    static constexpr size_t BLOB_ITERS    = 20000;

    struct blob_writer_args
    {
        minstd::lockfree::latest_value_blob<BLOB_MAXBYTES> *blob;
        uint32_t                                           id;
        minstd::atomic<uint32_t>                           wins{0};
    };

    void *blob_writer_thread(void *arg)
    {
        blob_writer_args *a = static_cast<blob_writer_args *>(arg);
        uint8_t buf[BLOB_MAXBYTES];

        for (uint32_t i = 1; i <= BLOB_ITERS; ++i)
        {
            const uint8_t v   = static_cast<uint8_t>(((a->id * 3u + i) % 250u) + 1u);
            const size_t  len = (i % BLOB_MAXBYTES) + 1;

            for (size_t k = 0; k < len; ++k)
            {
                buf[k] = v;
            }

            if (a->blob->store(buf, len) == latest_value_result::SUCCESS)
            {
                a->wins.fetch_add(1, minstd::memory_order_relaxed);
            }
        }

        return nullptr;
    }

    struct blob_reader_args
    {
        minstd::lockfree::latest_value_blob<BLOB_MAXBYTES> *blob;
        minstd::atomic<bool>      stop{false};
        minstd::atomic<uint64_t> reads{0};
        minstd::atomic<uint64_t> torn{0};
    };

    static bool blob_is_consistent(const uint8_t *d, size_t max) noexcept
    {
        size_t b = 0;

        while (b < max && d[b] != 0)
        {
            ++b;   //  b = first zero byte (the stored length)
        }

        if (b == 0)
        {
            return true;   //  all zero
        }

        const uint8_t v = d[0];

        for (size_t i = 1; i < b; ++i)
        {
            if (d[i] != v)
            {
                return false;   //  data region not uniform
            }
        }

        for (size_t i = b; i < max; ++i)
        {
            if (d[i] != 0)
            {
                return false;   //  tail not zero
            }
        }

        return true;
    }

    void *blob_reader_thread(void *arg)
    {
        blob_reader_args *a = static_cast<blob_reader_args *>(arg);
        uint8_t dst[BLOB_MAXBYTES];

        while (!a->stop.load(minstd::memory_order_acquire))
        {
            memset(dst, 0, sizeof(dst));
            a->blob->load(dst, sizeof(dst));
            a->reads.fetch_add(1, minstd::memory_order_relaxed);

            if (!blob_is_consistent(dst, sizeof(dst)))
            {
                a->torn.fetch_add(1, minstd::memory_order_relaxed);
            }
        }

        return nullptr;
    }

    TEST(LatestValueSlotArrayTests, BlobMultiThreadedWritersAndReaders)
    {
        minstd::lockfree::latest_value_blob<BLOB_MAXBYTES> blob;

        blob_reader_args rargs;
        rargs.blob = &blob;
        pthread_t readers[BLOB_READERS];

        for (size_t i = 0; i < BLOB_READERS; ++i)
        {
            pthread_create(&readers[i], nullptr, blob_reader_thread, &rargs);
        }

        blob_writer_args wargs[BLOB_WRITERS];
        pthread_t writers[BLOB_WRITERS];

        for (size_t i = 0; i < BLOB_WRITERS; ++i)
        {
            wargs[i].blob = &blob;
            wargs[i].id   = static_cast<uint32_t>(i);
            pthread_create(&writers[i], nullptr, blob_writer_thread, &wargs[i]);
        }

        for (size_t i = 0; i < BLOB_WRITERS; ++i)
        {
            pthread_join(writers[i], nullptr);
        }

        for (size_t i = 0; i < BLOB_READERS; ++i)
        {
            rargs.stop.store(true, minstd::memory_order_release);
        }
        
        for (size_t i = 0; i < BLOB_READERS; ++i)
        {
            pthread_join(readers[i], nullptr);
        }

        CHECK_EQUAL(0ull, rargs.torn.load());
        CHECK_TRUE(total_wins(wargs, BLOB_WRITERS) >= 1);       //  see MultiThreadedWritersAndReaders

        uint8_t dst[BLOB_MAXBYTES];
        memset(dst, 0, sizeof(dst));
        blob.load(dst, sizeof(dst));
        CHECK_TRUE(blob_is_consistent(dst, sizeof(dst)));
    }
}
