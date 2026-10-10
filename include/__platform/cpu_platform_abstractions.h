// Copyright 2025 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  This file contains conditional compilation for different CPU/OS combinations
//      primarily either X64 or ARM64 on linux to permit unit and performance
//      testing of lockfree classes, like the lockfree memory allocator.
//
//  This should be the only location in the project with this type of platform
//      specific code, so if porting to a new CPU or OS this ought to be the
//      only place to add new code.

#pragma once

#include "minstdconfig.h"
#include <stdint.h>

namespace MINIMAL_STD_NAMESPACE
{
    namespace platform
    {
#if defined(__MINIMAL_STD_TEST__)
        using test_cpu_id_provider_type = uint32_t (*)();
        inline test_cpu_id_provider_type test_cpu_id_provider_ = nullptr;

        inline void set_test_cpu_id_provider(test_cpu_id_provider_type provider)
        {
            test_cpu_id_provider_ = provider;
        }

        inline void clear_test_cpu_id_provider()
        {
            test_cpu_id_provider_ = nullptr;
        }
#endif

#if defined(__x86_64__) || defined(_M_X64)
        inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t &eax, uint32_t &ebx, uint32_t &ecx, uint32_t &edx)
        {
            __asm__ volatile(
                "cpuid"
                : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "a"(leaf), "c"(subleaf));
        }
#endif


        /**
         * @brief Returns a monotonically increasing counter value.
         *
         * This function provides a high-resolution, monotonically increasing counter
         * suitable for ordering events in lock-free algorithms. The counter is per-CPU
         * and may not be synchronized across CPUs, but is guaranteed to be monotonic
         * on any single CPU.
         *
         * On x64: Uses RDTSC (Time Stamp Counter)
         * On ARM64: Uses CNTVCT_EL0 (Counter-timer Virtual Count)
         *
         * @return A 64-bit monotonically increasing counter value.
         */
        inline uint64_t get_monotonic_counter()
        {
#if defined(__x86_64__) || defined(_M_X64)
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            return (static_cast<uint64_t>(hi) << 32) | lo;

#elif defined(__aarch64__)
            uint64_t counter;
            __asm__ volatile("mrs %0, cntvct_el0" : "=r"(counter));
            return counter;

#else
#error "Unsupported architecture for get_monotonic_counter()"
#endif
        }

        /**
         * @brief Decodes an AArch64 MPIDR_EL1 value into a CPU id: the core within its cluster, with the cluster
         *        above it.
         *
         * MPIDR_EL1.MT (bit 24) says whether Aff0 is a hardware thread.  When it is set (Cortex-A55/A65/A75 and
         * later, Neoverse), Aff0 is the thread within the core - 0 on every core of a single-threaded part - so
         * the core is Aff1 and the cluster Aff2.  Otherwise the core is Aff0 and the cluster Aff1.  Taking Aff0
         * alone gave every core the same id on the MT parts.  Each field is 8 bits, so the result is unique per
         * core.  Separate from get_cpu_id() so the decoding can be tested on any host.
         */
        constexpr uint32_t cpu_id_from_mpidr(uint64_t mpidr)
        {
            const bool multithreaded = ((mpidr >> 24) & 0x1) != 0;
            const uint32_t aff0 = static_cast<uint32_t>(mpidr & 0xFF);
            const uint32_t aff1 = static_cast<uint32_t>((mpidr >> 8) & 0xFF);
            const uint32_t aff2 = static_cast<uint32_t>((mpidr >> 16) & 0xFF);

            const uint32_t core = multithreaded ? aff1 : aff0;
            const uint32_t cluster = multithreaded ? aff2 : aff1;

            return core | (cluster << 8);
        }

        /**
         * @brief Returns the current CPU/core ID.
         *
         * This function returns an identifier for the current CPU core. This can be
         * used for per-CPU sharding to reduce contention in multi-core systems.
         *
         * On x64: Uses CPUID instruction with leaf 0x1 (returns initial APIC ID)
         * On ARM64: Uses MPIDR_EL1 (Multiprocessor Affinity Register), see cpu_id_from_mpidr()
         *
         * Note: The returned value may not be contiguous (0, 1, 2, ...) but is
         * guaranteed to be unique per CPU core.
         *
         * @return A CPU identifier value.
         */
        inline uint32_t get_cpu_id()
        {
#if defined(__MINIMAL_STD_TEST__)
            if (test_cpu_id_provider_ == nullptr)
            {
                MINIMAL_STD_FAIL(test_cpu_id_provider_not_set_for_test_mode);
                return 0u;
            }

            return test_cpu_id_provider_();

#elif defined(__x86_64__) || defined(_M_X64)
            uint32_t ebx;
            __asm__ volatile(
                "movl $1, %%eax\n\t"
                "cpuid"
                : "=b"(ebx)
                :
                : "eax", "ecx", "edx");
            // Initial APIC ID is in bits 31:24 of EBX
            return (ebx >> 24) & 0xFF;

#elif defined(__aarch64__)
            uint64_t mpidr;
            __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

            return cpu_id_from_mpidr(mpidr);

#else
#error "Unsupported architecture for get_cpu_id()"
#endif
        }

        /**
         * @brief Returns the number of available CPU cores.
         *
         * On Linux: Uses sysconf(_SC_NPROCESSORS_ONLN).
         * On x64 bare-metal: Uses CPUID topology leaves when available.
         * On ARM64 bare-metal: Returns 1.
         * On other platforms: Returns 1.
         *
         * @return Number of online CPU cores (>= 1).
         */
        inline uint32_t get_cpu_count()
        {
#if defined(__MINIMAL_STD_TEST__)
            return 1u;

#elif defined(__x86_64__) || defined(_M_X64)
            uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
            cpuid(0, 0, eax, ebx, ecx, edx);
            uint32_t max_leaf = eax;

            if (max_leaf >= 0x0B)
            {
                uint32_t logical_count = 0;

                for (uint32_t level = 0; level < 8; ++level)
                {
                    cpuid(0x0B, level, eax, ebx, ecx, edx);
                    uint32_t level_type = (ecx >> 8) & 0xFF;
                    if (level_type == 0)
                    {
                        break;
                    }

                    if (level_type == 2)
                    {
                        logical_count = ebx & 0xFFFF;
                        break;
                    }
                }

                if (logical_count != 0)
                {
                    return logical_count;
                }
            }

            if (max_leaf >= 0x01)
            {
                cpuid(0x01, 0, eax, ebx, ecx, edx);
                uint32_t logical_count = (ebx >> 16) & 0xFF;
                return (logical_count != 0) ? logical_count : 1u;
            }

            return 1u;
#else
            return 1u;
#endif
        }

        /**
         * @brief Hint to the CPU that we are in a spin-wait loop.
         *
         * This can reduce power or contention while spinning. Note that on modern
         * x86_64 architectures (Skylake and newer), PAUSE takes ~140 cycles, whereas
         * on AArch64, YIELD is typically much faster. Use the back_off() function
         * which normalizes these latency differences.
         */
        inline void cpu_relax()
        {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#else
            __asm__ __volatile__("" ::: "memory");
#endif
        }

        /**
         * @brief Standardized exponential backoff for lock-free contention.
         *
         * On x64, PAUSE is ~140 cycles on modern architectures, so we use a small multiplier.
         * On AArch64, YIELD is much faster, so we use a larger multiplier.
         */
        inline void back_off(size_t &retries)
        {
            const size_t bounded_retries = (retries > 256) ? 256 : retries;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
            const size_t spin_count = 2 + (bounded_retries * 2);
#else
            const size_t spin_count = 32 + (bounded_retries * 32);
#endif
            for (size_t i = 0; i < spin_count; ++i)
            {
                cpu_relax();
            }

            retries++;
        }

        /**
         * @brief Check whether two consecutive 64-bit words are both all ones (~0ULL).
         *
         * Used for optimistic scanning in lock-free bitset structures.  Plain scalar code: for
         * two words it is as fast as SIMD (x64: two loads, AND, compare; ARM64: LDP, AND,
         * compare), needs no intrinsics header, and has no alignment requirement.
         *
         * @param chunk_ptr Pointer to two consecutive uint64_t values
         * @return true if both words are all ones (~0ULL), false otherwise
         */
        inline bool simd_scan_128bit_is_all_ones(const uint64_t *chunk_ptr)
        {
            return (chunk_ptr[0] & chunk_ptr[1]) == ~0ULL;
        }

        struct default_platform_provider
        {
            static inline uint32_t get_cpu_id()
            {
                return platform::get_cpu_id();
            }

            static inline uint64_t get_monotonic_counter()
            {
                return platform::get_monotonic_counter();
            }

            static inline void cpu_relax()
            {
                platform::cpu_relax();
            }

            static inline void back_off(size_t &retries)
            {
                platform::back_off(retries);
            }
        };

    } // namespace platform
} // namespace MINIMAL_STD_NAMESPACE
