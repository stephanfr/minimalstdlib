// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <mutex>

#include <stddef.h>
#include <stdint.h>

namespace
{
    //
    //  Captures contract violations.  This binary is built with MINIMAL_STD_FAILURE_POLICY_TEST,
    //      so contract_violation() reports through the failure hook and returns instead of trapping.
    //

    struct mutex_failure_event_record
    {
        const char *reason = nullptr;
        const char *message = nullptr;
        int call_count = 0;
    };

    mutex_failure_event_record g_mutex_failure_event;

    void record_mutex_failure_event(const char *reason,
                                    const char *message,
                                    const char *file,
                                    int line)
    {
        (void)file;
        (void)line;

        g_mutex_failure_event.reason = reason;
        g_mutex_failure_event.message = message;
        g_mutex_failure_event.call_count++;
    }

    bool message_contains(const char *text, const char *needle)
    {
        if ((text == nullptr) || (needle == nullptr))
        {
            return false;
        }

        for (const char *start = text; *start != '\0'; ++start)
        {
            const char *candidate = start;
            const char *pattern = needle;

            while ((*candidate != '\0') && (*pattern != '\0') && (*candidate == *pattern))
            {
                ++candidate;
                ++pattern;
            }

            if (*pattern == '\0')
            {
                return true;
            }
        }

        return false;
    }

    //  There is only one thread here, so the recursive mutex's owner is simulated: the policy
    //      returns whatever the test sets, letting the test act as two different owners.

    uintptr_t g_simulated_owner = 1;

    struct simulated_owner_policy
    {
        static uintptr_t current_owner() noexcept
        {
            return g_simulated_owner;
        }
    };

    using test_recursive_mutex = minstd::recursive_spin_mutex<simulated_owner_policy>;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (MutexFailureModeTests)
    {
        minstd::failure_hook_type previous_hook_ = nullptr;

        void setup() override
        {
            g_mutex_failure_event = mutex_failure_event_record{};
            g_simulated_owner = 1;

            previous_hook_ = minstd::set_failure_hook(record_mutex_failure_event);
        }

        void teardown() override
        {
            minstd::set_failure_hook(previous_hook_);
        }
    };
#pragma GCC diagnostic pop

    //
    //  recursive_spin_mutex
    //

    TEST(MutexFailureModeTests, RecursiveMutexUnlockByNonOwnerIsAContractViolation)
    {
        test_recursive_mutex mutex;

        mutex.lock();                                   //  owner 1

        g_simulated_owner = 2;
        mutex.unlock();                                 //  not owner 2's to release

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
        STRCMP_EQUAL("contract_violation", g_mutex_failure_event.reason);
        CHECK(message_contains(g_mutex_failure_event.message, "recursive_spin_mutex"));

        //  The violation left the lock alone: owner 1 still holds it.

        CHECK_FALSE(mutex.try_lock());

        g_simulated_owner = 1;
        mutex.unlock();

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
    }

    TEST(MutexFailureModeTests, RecursiveMutexUnlockWhileUnheldIsAContractViolation)
    {
        test_recursive_mutex mutex;

        mutex.unlock();

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
        CHECK(message_contains(g_mutex_failure_event.message, "recursive_spin_mutex"));

        //  Still usable afterwards.

        CHECK(mutex.try_lock());
        mutex.unlock();
        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
    }

    //
    //  unique_lock
    //

    TEST(MutexFailureModeTests, UniqueLockUnlockWithoutOwningIsAContractViolation)
    {
        minstd::spin_mutex mutex;
        minstd::unique_lock lock(mutex, minstd::defer_lock);

        lock.unlock();

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
        STRCMP_EQUAL("contract_violation", g_mutex_failure_event.reason);
        CHECK(message_contains(g_mutex_failure_event.message, "unique_lock::unlock"));

        CHECK(mutex.try_lock());                        //  the mutex was not touched
        mutex.unlock();
    }

    TEST(MutexFailureModeTests, UniqueLockLockWithoutAMutexIsAContractViolation)
    {
        minstd::unique_lock<minstd::spin_mutex> lock;

        lock.lock();

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
        CHECK(message_contains(g_mutex_failure_event.message, "unique_lock::lock"));
        CHECK_FALSE(lock.owns_lock());
    }

    TEST(MutexFailureModeTests, UniqueLockTryLockWithoutAMutexIsAContractViolation)
    {
        minstd::unique_lock<minstd::spin_mutex> lock;

        CHECK_FALSE(lock.try_lock());

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
        CHECK(message_contains(g_mutex_failure_event.message, "unique_lock::try_lock"));
        CHECK_FALSE(lock.owns_lock());
    }

    TEST(MutexFailureModeTests, UniqueLockRelockWhileOwningIsAContractViolation)
    {
        minstd::spin_mutex mutex;
        minstd::unique_lock lock(mutex);

        lock.lock();                                    //  on a spin_mutex this would deadlock

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
        CHECK(message_contains(g_mutex_failure_event.message, "unique_lock::lock"));
        CHECK(lock.owns_lock());                        //  still owns it - once
    }

    TEST(MutexFailureModeTests, UniqueLockTryLockWhileOwningIsAContractViolation)
    {
        minstd::spin_mutex mutex;
        minstd::unique_lock lock(mutex);

        CHECK_FALSE(lock.try_lock());

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
        CHECK(message_contains(g_mutex_failure_event.message, "unique_lock::try_lock"));
        CHECK(lock.owns_lock());
    }

    TEST(MutexFailureModeTests, UniqueLockUnlockAfterReleaseIsAContractViolation)
    {
        minstd::spin_mutex mutex;
        minstd::unique_lock lock(mutex);

        minstd::spin_mutex *released = lock.release();

        lock.unlock();                                  //  ownership went with release()

        CHECK_EQUAL(1, g_mutex_failure_event.call_count);
        CHECK(message_contains(g_mutex_failure_event.message, "unique_lock::unlock"));

        CHECK_FALSE(mutex.try_lock());                  //  still held: the caller owns it now
        released->unlock();
    }
}