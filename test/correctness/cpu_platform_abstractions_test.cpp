// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>
#include <__platform/cpu_platform_abstractions.h>

#include <stdint.h>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(CpuPlatformAbstractionsTests)
    {
    };
#pragma GCC diagnostic pop

    constexpr uint64_t MPIDR_MT = uint64_t{1} << 24;
    constexpr uint64_t MPIDR_RES1 = uint64_t{1} << 31;

    constexpr uint64_t mpidr(uint32_t aff2, uint32_t aff1, uint32_t aff0, bool mt)
    {
        return MPIDR_RES1 | (mt ? MPIDR_MT : 0) | (uint64_t{aff2} << 16) | (uint64_t{aff1} << 8) | aff0;
    }

    TEST(CpuPlatformAbstractionsTests, MpidrDecodingGivesEachCoreItsOwnId)
    {
        using minstd::platform::cpu_id_from_mpidr;

        //  Cortex-A76 / Neoverse N1 (MT = 1, Aff0 = thread = 0): the core is Aff1.  Before: every core was 0.
        CHECK_EQUAL(0u, cpu_id_from_mpidr(mpidr(0, 0, 0, true)));
        CHECK_EQUAL(1u, cpu_id_from_mpidr(mpidr(0, 1, 0, true)));
        CHECK_EQUAL(3u, cpu_id_from_mpidr(mpidr(0, 3, 0, true)));
        CHECK_EQUAL(0x102u, cpu_id_from_mpidr(mpidr(1, 2, 0, true)));

        //  Cortex-A53 / A72 (MT = 0): the core is Aff0, the cluster Aff1.  Before: clusters collided.
        CHECK_EQUAL(2u, cpu_id_from_mpidr(mpidr(0, 0, 2, false)));
        CHECK_EQUAL(0x101u, cpu_id_from_mpidr(mpidr(0, 1, 1, false)));

        CHECK(cpu_id_from_mpidr(mpidr(0, 0, 1, false)) != cpu_id_from_mpidr(mpidr(0, 1, 1, false)));
    }
}
