// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <atomic>
#include <mutex>
#include <utility>

#include <pthread.h>
#include <sched.h>
#include <stdint.h>

namespace
{
#ifdef __SANITIZE_THREAD__
    static constexpr size_t MUTEX_STRESS_NUM_THREADS = 4;
    static constexpr size_t MUTEX_STRESS_ITERATIONS_PER_THREAD = 10000;
#else
    static constexpr size_t MUTEX_STRESS_NUM_THREADS = 8;
    static constexpr size_t MUTEX_STRESS_ITERATIONS_PER_THREAD = 100000;
#endif

    //  Owner identity for recursive_spin_mutex: the address of a thread_local is unique for each
    //      live thread and never 0, whatever type pthread_t happens to be.

    struct pthread_owner_policy
    {
        static uintptr_t current_owner() noexcept
        {
            static thread_local char marker;

            return reinterpret_cast<uintptr_t>(&marker);
        }
    };

    using test_recursive_mutex = minstd::recursive_spin_mutex<pthread_owner_policy>;

    //
    //  Stress-test state.  counter_ is deliberately not atomic - only the lock protects it.
    //      occupants_ counts threads inside the critical section; seeing anyone else already
    //      there is a mutual-exclusion failure, counted in violations_.
    //

    template <typename Mutex>
    struct mutex_stress_state
    {
        Mutex mutex_;
        uint64_t counter_ = 0;

        minstd::atomic<uint32_t> occupants_{0};
        minstd::atomic<size_t> violations_{0};

        minstd::atomic<bool> start_{false};
        minstd::atomic<size_t> ready_count_{0};
    };

    template <typename Mutex>
    void wait_for_start(mutex_stress_state<Mutex> &state)
    {
        state.ready_count_.fetch_add(1, minstd::memory_order_release);

        while (!state.start_.load(minstd::memory_order_acquire))
        {
            sched_yield();
        }
    }

    template <typename Mutex>
    void critical_section(mutex_stress_state<Mutex> &state)
    {
        if (state.occupants_.fetch_add(1, minstd::memory_order_acq_rel) != 0)
        {
            state.violations_.fetch_add(1, minstd::memory_order_relaxed);
        }

        state.counter_++;

        state.occupants_.fetch_sub(1, minstd::memory_order_acq_rel);
    }

    //
    //  Workers
    //

    void *spin_mutex_lock_guard_worker(void *arg)
    {
        auto &state = *static_cast<mutex_stress_state<minstd::spin_mutex> *>(arg);

        wait_for_start(state);

        for (size_t i = 0; i < MUTEX_STRESS_ITERATIONS_PER_THREAD; ++i)
        {
            minstd::lock_guard guard(state.mutex_);

            critical_section(state);
        }

        return nullptr;
    }

    void *spin_mutex_try_lock_worker(void *arg)
    {
        auto &state = *static_cast<mutex_stress_state<minstd::spin_mutex> *>(arg);

        wait_for_start(state);

        size_t acquired = 0;

        while (acquired < MUTEX_STRESS_ITERATIONS_PER_THREAD)
        {
            minstd::unique_lock lock(state.mutex_, minstd::try_to_lock);

            if (lock)
            {
                critical_section(state);
                acquired++;
            }
            else
            {
                minstd::platform::cpu_relax();
            }
        }

        return nullptr;
    }

    void *recursive_mutex_nested_worker(void *arg)
    {
        auto &state = *static_cast<mutex_stress_state<test_recursive_mutex> *>(arg);

        wait_for_start(state);

        for (size_t i = 0; i < MUTEX_STRESS_ITERATIONS_PER_THREAD; ++i)
        {
            minstd::lock_guard outer(state.mutex_);
            minstd::unique_lock middle(state.mutex_);

            {
                minstd::lock_guard inner(state.mutex_);

                critical_section(state);            //  depth 3
            }

            middle.unlock();

            critical_section(state);                //  depth 1: still held by outer
        }

        return nullptr;
    }

    template <typename Mutex>
    void run_stress_threads(mutex_stress_state<Mutex> &state, void *(*worker)(void *))
    {
        pthread_t workers[MUTEX_STRESS_NUM_THREADS]{};

        for (size_t i = 0; i < MUTEX_STRESS_NUM_THREADS; ++i)
        {
            CHECK_EQUAL(0, pthread_create(&workers[i], nullptr, worker, &state));
        }

        while (state.ready_count_.load(minstd::memory_order_acquire) < MUTEX_STRESS_NUM_THREADS)
        {
            sched_yield();
        }

        state.start_.store(true, minstd::memory_order_release);

        for (size_t i = 0; i < MUTEX_STRESS_NUM_THREADS; ++i)
        {
            CHECK_EQUAL(0, pthread_join(workers[i], nullptr));
        }
    }

    //
    //  Asks another thread to try the lock - how ownership is observed from outside
    //

    template <typename Mutex>
    void *try_lock_from_other_thread(void *arg)
    {
        auto *mutex = static_cast<Mutex *>(arg);

        const bool acquired = mutex->try_lock();

        if (acquired)
        {
            mutex->unlock();
        }

        return acquired ? arg : nullptr;
    }

    template <typename Mutex>
    bool other_thread_can_lock(Mutex &mutex)
    {
        pthread_t thread{};
        void *result = nullptr;

        CHECK_EQUAL(0, pthread_create(&thread, nullptr, try_lock_from_other_thread<Mutex>, &mutex));
        CHECK_EQUAL(0, pthread_join(thread, &result));

        return result != nullptr;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (MutexTests)
    {
    };
#pragma GCC diagnostic pop

    //
    //  spin_mutex
    //

    TEST(MutexTests, SpinMutexTryLockFailsWhileHeld)
    {
        minstd::spin_mutex mutex;

        CHECK(mutex.try_lock());
        CHECK_FALSE(mutex.try_lock());

        mutex.unlock();

        CHECK(mutex.try_lock());
        mutex.unlock();
    }

    TEST(MutexTests, SpinMutexExcludesOtherThreads)
    {
        minstd::spin_mutex mutex;

        mutex.lock();
        CHECK_FALSE(other_thread_can_lock(mutex));

        mutex.unlock();
        CHECK(other_thread_can_lock(mutex));
    }

    TEST(MutexTests, SpinMutexCanBeConstantInitialised)
    {
        //  Like std::mutex: usable as a global with no startup-ordering hazard.

        static constinit minstd::spin_mutex global_mutex;

        CHECK(global_mutex.try_lock());
        global_mutex.unlock();
    }

    //
    //  lock_guard
    //

    TEST(MutexTests, LockGuardHoldsForItsScope)
    {
        minstd::spin_mutex mutex;

        {
            minstd::lock_guard guard(mutex);

            CHECK_FALSE(other_thread_can_lock(mutex));
        }

        CHECK(other_thread_can_lock(mutex));
    }

    TEST(MutexTests, LockGuardAdoptsAHeldLock)
    {
        minstd::spin_mutex mutex;

        mutex.lock();

        {
            minstd::lock_guard guard(mutex, minstd::adopt_lock);

            CHECK_FALSE(other_thread_can_lock(mutex));
        }

        CHECK(other_thread_can_lock(mutex));        //  the guard released the adopted lock
    }

    //
    //  unique_lock
    //

    TEST(MutexTests, UniqueLockLocksOnConstruction)
    {
        minstd::spin_mutex mutex;

        {
            minstd::unique_lock lock(mutex);

            CHECK(lock.owns_lock());
            CHECK(static_cast<bool>(lock));
            CHECK(lock.mutex() == &mutex);
            CHECK_FALSE(other_thread_can_lock(mutex));
        }

        CHECK(other_thread_can_lock(mutex));
    }

    TEST(MutexTests, UniqueLockDeferThenLockAndUnlock)
    {
        minstd::spin_mutex mutex;

        minstd::unique_lock lock(mutex, minstd::defer_lock);

        CHECK_FALSE(lock.owns_lock());
        CHECK(other_thread_can_lock(mutex));        //  deferred: the mutex is still free

        lock.lock();
        CHECK(lock.owns_lock());
        CHECK_FALSE(other_thread_can_lock(mutex));

        lock.unlock();
        CHECK_FALSE(lock.owns_lock());
        CHECK(other_thread_can_lock(mutex));
    }

    TEST(MutexTests, UniqueLockTryToLock)
    {
        minstd::spin_mutex mutex;

        {
            minstd::unique_lock first(mutex, minstd::try_to_lock);
            CHECK(first.owns_lock());

            minstd::unique_lock second(mutex, minstd::try_to_lock);
            CHECK_FALSE(second.owns_lock());
        }

        CHECK(other_thread_can_lock(mutex));
    }

    TEST(MutexTests, UniqueLockMoveTransfersOwnership)
    {
        minstd::spin_mutex mutex;

        minstd::unique_lock source(mutex);
        minstd::unique_lock target(minstd::move(source));

        CHECK_FALSE(source.owns_lock());
        CHECK(source.mutex() == nullptr);
        CHECK(target.owns_lock());
        CHECK(target.mutex() == &mutex);

        //  Move-assignment releases what the target held first.

        minstd::spin_mutex other;
        minstd::unique_lock other_lock(other);

        target = minstd::move(other_lock);

        CHECK(other_thread_can_lock(mutex));        //  released by the assignment
        CHECK(target.mutex() == &other);
        CHECK_FALSE(other_thread_can_lock(other));
    }

    TEST(MutexTests, UniqueLockSwapAndRelease)
    {
        minstd::spin_mutex first_mutex;
        minstd::spin_mutex second_mutex;

        minstd::unique_lock first(first_mutex);
        minstd::unique_lock second(second_mutex, minstd::defer_lock);

        first.swap(second);

        CHECK(first.mutex() == &second_mutex);
        CHECK_FALSE(first.owns_lock());
        CHECK(second.mutex() == &first_mutex);
        CHECK(second.owns_lock());

        //  release() hands the still-locked mutex back to the caller.

        minstd::spin_mutex *released = second.release();

        CHECK(released == &first_mutex);
        CHECK(second.mutex() == nullptr);
        CHECK_FALSE(second.owns_lock());
        CHECK_FALSE(other_thread_can_lock(first_mutex));

        released->unlock();
        CHECK(other_thread_can_lock(first_mutex));
    }

    //
    //  recursive_spin_mutex
    //

    TEST(MutexTests, RecursiveMutexNestsForItsOwnerAndExcludesOthers)
    {
        test_recursive_mutex mutex;

        mutex.lock();
        mutex.lock();
        CHECK(mutex.try_lock());                    //  depth 3: re-entry by the owner succeeds

        CHECK_FALSE(other_thread_can_lock(mutex));

        mutex.unlock();
        mutex.unlock();

        CHECK_FALSE(other_thread_can_lock(mutex));  //  depth 1: still held

        mutex.unlock();

        CHECK(other_thread_can_lock(mutex));        //  depth 0: released
    }

    //
    //  Multithreaded stress - every thread contends for one lock from a common start
    //

    TEST(MutexTests, SpinMutexSerialisesThreadsWithLockGuard)
    {
        mutex_stress_state<minstd::spin_mutex> state;

        run_stress_threads(state, spin_mutex_lock_guard_worker);

        CHECK_EQUAL(static_cast<uint64_t>(MUTEX_STRESS_NUM_THREADS * MUTEX_STRESS_ITERATIONS_PER_THREAD), state.counter_);
        CHECK_EQUAL(static_cast<size_t>(0), state.violations_.load(minstd::memory_order_acquire));
    }

    TEST(MutexTests, SpinMutexSerialisesThreadsWithTryLock)
    {
        mutex_stress_state<minstd::spin_mutex> state;

        run_stress_threads(state, spin_mutex_try_lock_worker);

        CHECK_EQUAL(static_cast<uint64_t>(MUTEX_STRESS_NUM_THREADS * MUTEX_STRESS_ITERATIONS_PER_THREAD), state.counter_);
        CHECK_EQUAL(static_cast<size_t>(0), state.violations_.load(minstd::memory_order_acquire));
    }

    TEST(MutexTests, RecursiveMutexSerialisesThreadsWithNestedLocks)
    {
        mutex_stress_state<test_recursive_mutex> state;

        run_stress_threads(state, recursive_mutex_nested_worker);

        //  Two critical sections per iteration.

        CHECK_EQUAL(static_cast<uint64_t>(MUTEX_STRESS_NUM_THREADS * MUTEX_STRESS_ITERATIONS_PER_THREAD * 2), state.counter_);
        CHECK_EQUAL(static_cast<size_t>(0), state.violations_.load(minstd::memory_order_acquire));
    }

    struct switchable_owner_policy
    {
        static inline uintptr_t current = 0;

        static uintptr_t current_owner() noexcept
        {
            return current;
        }
    };

    TEST(MutexTests, RecursiveSpinMutexOwnerIdZeroReallyLocks)
    {
        //  owner_ used 0 for "unowned", so a caller whose id is 0 (core 0, when the owner is the CPU id) took the
        //      recursion branch on its first lock() and never acquired the inner mutex.
        minstd::recursive_spin_mutex<switchable_owner_policy> mutex;

        switchable_owner_policy::current = 0;
        mutex.lock();

        switchable_owner_policy::current = 1; //  a different owner must be excluded
        CHECK_FALSE(mutex.try_lock());        //  before: succeeds - both "own" the mutex

        switchable_owner_policy::current = 0;
        CHECK_TRUE(mutex.try_lock()); //  recursion still works for owner 0
        mutex.unlock();
        mutex.unlock();

        switchable_owner_policy::current = 1;
        CHECK_TRUE(mutex.try_lock());
        mutex.unlock();
    }
}
