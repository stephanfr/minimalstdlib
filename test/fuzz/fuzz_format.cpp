// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  Fuzz the format-string parser and the formatters: the input is the format string, applied to a fixed set of
//      arguments of every formatted kind, into buffers small and large.  Crashes, sanitizer reports and hangs fail.

#include <minstdconfig.h>

#include <fixed_string>
#include <format>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char format_text[256];

    const size_t length = (size < sizeof(format_text) - 1) ? size : sizeof(format_text) - 1;

    for (size_t i = 0; i < length; i++)
    {
        format_text[i] = (data[i] != 0) ? static_cast<char>(data[i]) : ' ';
    }

    format_text[length] = 0;

    minstd::fixed_string<8> tiny;
    minstd::fixed_string<256> large;

    const int32_t i32 = -12345;
    const uint64_t u64 = 0xFEDCBA9876543210ULL;
    const double d = -1234.5678;
    const char *text = "text";
    void *pointer = reinterpret_cast<void *>(0x1234);

    minstd::format(tiny, format_text, i32, u64, d, text, pointer, true, 'c', (int8_t)-8, (uint16_t)65535, 1e300);
    minstd::format(large, format_text, i32, u64, d, text, pointer, true, 'c', (int8_t)-8, (uint16_t)65535, 1e300);

    return 0;
}
