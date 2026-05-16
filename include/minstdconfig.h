// Copyright 2023 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

static_assert(__cplusplus >= 202002L, "Minimal C++ Stdlib requires C++ 20 or later");          //  The library assume C++ 20 or better

#include <stddef.h>
#include <stdint.h>

#if !defined(MINIMAL_STD_ALLOCATION_FAILURE_POLICY)
#define MINIMAL_STD_ALLOCATION_FAILURE_POLICY 0
#endif

#define MINIMAL_STD_ALLOCATION_FAILURE_POLICY_LEGACY 0
#define MINIMAL_STD_ALLOCATION_FAILURE_POLICY_TRAP 1
#define MINIMAL_STD_ALLOCATION_FAILURE_POLICY_THROW 2

#if !defined(MINIMAL_STD_FAILURE_POLICY)
#define MINIMAL_STD_FAILURE_POLICY 0
#endif

#define MINIMAL_STD_FAILURE_POLICY_DEFAULT 0
#define MINIMAL_STD_FAILURE_POLICY_TEST 1

extern "C" void __assert(const char *msg, const char *file, int line);

#define MINIMAL_STD_NAMESPACE minstd
#define FMT_FORMATTERS_NAMESPACE fmt_formatters

namespace MINIMAL_STD_NAMESPACE
{
    using failure_hook_type = void (*)(const char *reason,
                                       const char *message,
                                       const char *file,
                                       int line);

    inline failure_hook_type &failure_hook_storage() noexcept
    {
        static failure_hook_type hook = nullptr;
        return hook;
    }

    inline failure_hook_type set_failure_hook(failure_hook_type hook) noexcept
    {
        failure_hook_type previous = failure_hook_storage();
        failure_hook_storage() = hook;
        return previous;
    }

    inline void invoke_failure_policy(const char *reason,
                                      const char *message,
                                      const char *file,
                                      int line)
    {
        failure_hook_type hook = failure_hook_storage();

        if (hook != nullptr)
        {
            hook(reason, message, file, line);
        }

    #if MINIMAL_STD_FAILURE_POLICY == MINIMAL_STD_FAILURE_POLICY_DEFAULT
        __assert(message, file, line);
    #elif MINIMAL_STD_FAILURE_POLICY == MINIMAL_STD_FAILURE_POLICY_TEST
        (void)reason;
        (void)message;
        (void)file;
        (void)line;
        return;
    #else
    #error "Invalid MINIMAL_STD_FAILURE_POLICY value"
    #endif
    }

    inline void contract_violation(const char *message,
                                   const char *file,
                                   int line)
    {
        invoke_failure_policy("contract_violation", message, file, line);
    }

    inline void allocation_failure(size_t requested_bytes,
                                   size_t alignment,
                                   const char *file,
                                   int line)
    {
        (void)requested_bytes;
        (void)alignment;

        failure_hook_type hook = failure_hook_storage();

        if (hook != nullptr)
        {
            hook("allocation_failure", "MINIMAL_STD_OUT_OF_MEMORY", file, line);
        }

#if MINIMAL_STD_FAILURE_POLICY == MINIMAL_STD_FAILURE_POLICY_DEFAULT
        __builtin_trap();
        __builtin_unreachable();
#elif MINIMAL_STD_FAILURE_POLICY == MINIMAL_STD_FAILURE_POLICY_TEST
        return;
#else
#error "Invalid MINIMAL_STD_FAILURE_POLICY value"
#endif
    }

    inline void invalid_optional_access(const char *file,
                                        int line)
    {
        failure_hook_type hook = failure_hook_storage();

        if (hook != nullptr)
        {
            hook("invalid_optional_access", "invalid_optional_access", file, line);
        }

#if MINIMAL_STD_FAILURE_POLICY == MINIMAL_STD_FAILURE_POLICY_DEFAULT
        __builtin_trap();
        __builtin_unreachable();
#elif MINIMAL_STD_FAILURE_POLICY == MINIMAL_STD_FAILURE_POLICY_TEST
        return;
#else
#error "Invalid MINIMAL_STD_FAILURE_POLICY value"
#endif
    }
}

#define MINIMAL_STD_ASSERT(expression)                                                  \
    {                                                                                   \
        if (!(expression))                                                              \
        {                                                                               \
            ::MINIMAL_STD_NAMESPACE::contract_violation(#expression, __FILE__, __LINE__); \
        }                                                                               \
    }

#define MINIMAL_STD_FAIL(message)                                                   \
    {                                                                               \
        ::MINIMAL_STD_NAMESPACE::contract_violation(#message, __FILE__, __LINE__); \
    }

#if MINIMAL_STD_ALLOCATION_FAILURE_POLICY == MINIMAL_STD_ALLOCATION_FAILURE_POLICY_LEGACY
#define MINIMAL_STD_OUT_OF_MEMORY(requested_bytes, alignment, ...) \
    {                                                               \
        (void)(requested_bytes);                                    \
        (void)(alignment);                                          \
        __VA_ARGS__;                                                \
    }
#elif MINIMAL_STD_ALLOCATION_FAILURE_POLICY == MINIMAL_STD_ALLOCATION_FAILURE_POLICY_TRAP
#define MINIMAL_STD_OUT_OF_MEMORY(requested_bytes, alignment, ...)                    \
    {                                                                                 \
        ::MINIMAL_STD_NAMESPACE::allocation_failure((requested_bytes), (alignment),   \
                                                    __FILE__, __LINE__);              \
    }
#elif MINIMAL_STD_ALLOCATION_FAILURE_POLICY == MINIMAL_STD_ALLOCATION_FAILURE_POLICY_THROW
#if defined(__cpp_exceptions)
#include <new>
#define MINIMAL_STD_OUT_OF_MEMORY(requested_bytes, alignment, ...) \
    {                                                               \
        (void)(requested_bytes);                                    \
        (void)(alignment);                                          \
        throw ::std::bad_alloc();                                  \
    }
#else
#define MINIMAL_STD_OUT_OF_MEMORY(requested_bytes, alignment, ...)                    \
    {                                                                                 \
        ::MINIMAL_STD_NAMESPACE::allocation_failure((requested_bytes), (alignment),   \
                                                    __FILE__, __LINE__);              \
    }
#endif
#else
#error "Invalid MINIMAL_STD_ALLOCATION_FAILURE_POLICY value"
#endif

#define _MINIMAL_STD_NODEBUG
#define _MINIMAL_STD_HIDDEN __attribute__((visibility("hidden")))

#define _MINIMAL_STD_PUSH_MACROS _Pragma("push_macro(\"min\")") _Pragma("push_macro(\"max\")") _Pragma("push_macro(\"refresh\")") _Pragma("push_macro(\"move\")") _Pragma("push_macro(\"erase\")")
#define _MINIMAL_STD_POP_MACROS _Pragma("pop_macro(\"min\")") _Pragma("pop_macro(\"max\")") _Pragma("pop_macro(\"refresh\")") _Pragma("pop_macro(\"move\")") _Pragma("pop_macro(\"erase\")")

#define _MINIMAL_STD_PROVIDE_COMPILER_STD_NAMESPACE_DEPENDENCIES

using nullptr_t = decltype(nullptr);

constexpr size_t DEFAULT_FIXED_STRING_MAX_SIZE = 64;

constexpr size_t DEFAULT_DYNAMIC_STRING_SIZE = 64;
constexpr size_t DEFAULT_DYNAMIC_STRING_MAX_SIZE = 16384;
constexpr double DYNAMIC_STRING_OVERALLOCATION_PERCENTAGE = 1.25;


