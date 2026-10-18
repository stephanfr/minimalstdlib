// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  A fuzz driver for builds without libFuzzer (g++, or FUZZ_ENGINE=standalone in run_fuzzers.sh).  It calls
//      the same LLVMFuzzerTestOneInput entry point with generated inputs: random bytes, bytes from a small dictionary
//      of format-spec and number tokens, and mutations of earlier inputs.  There is no coverage feedback, so it finds
//      less than libFuzzer would per execution; it makes up for some of that in speed.
//
//    driver SECONDS SEED         generate for SECONDS
//    driver FILE ...             replay inputs (e.g. a saved crash)
//
//  Each input is written to last_input.bin before it runs, so a crash leaves its input behind.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace
{
    uint64_t state;

    uint64_t next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    const char *const TOKENS[] = {"{", "}", "{}", "{:", ":", "<", ">", "^", "#", "+", "-", " ", "0", ".", "x", "X", "b", "B",
                                  "o", "d", "p", "*", "1", "9", "18", "255", "4294967296", "99999999999999999999", "z", "Z",
                                  "{{", "}}", "{0}", "{1:", "{:#010x}", "{:.17}"};

    size_t generate(uint8_t *out, size_t capacity, const uint8_t *previous, size_t previous_size)
    {
        size_t n = 0;

        switch (next() % 3)
        {
        case 0: //  random bytes
        {
            const size_t length = next() % capacity;
            for (; n < length; n++) out[n] = static_cast<uint8_t>(next());
            break;
        }

        case 1: //  dictionary tokens
        {
            const size_t tokens = next() % 24;
            for (size_t t = 0; t < tokens; t++)
            {
                const char *token = TOKENS[next() % (sizeof(TOKENS) / sizeof(TOKENS[0]))];
                for (const char *c = token; *c && (n < capacity); c++) out[n++] = static_cast<uint8_t>(*c);
            }
            break;
        }

        default: //  mutate the previous input
        {
            for (; n < previous_size; n++) out[n] = previous[n];
            const size_t edits = 1 + next() % 8;
            for (size_t e = 0; e < edits; e++)
            {
                const size_t kind = next() % 3;
                if ((kind == 0) && (n > 0)) out[next() % n] = static_cast<uint8_t>(next());
                else if ((kind == 1) && (n + 1 < capacity)) out[n++] = static_cast<uint8_t>(next());
                else if (n > 0) n--;
            }
            break;
        }
        }

        return n;
    }

    void save(const uint8_t *data, size_t size)
    {
        FILE *f = fopen("last_input.bin", "wb");
        if (f != nullptr)
        {
            fwrite(data, 1, size, f);
            fclose(f);
        }
    }
}

int main(int argc, char **argv)
{
    char *end = nullptr;
    const long seconds = (argc > 1) ? strtol(argv[1], &end, 10) : 60;

    if ((argc > 1) && (end != nullptr) && (*end != 0))
    {
        //  Replay files

        for (int i = 1; i < argc; i++)
        {
            FILE *f = fopen(argv[i], "rb");
            if (f == nullptr) { perror(argv[i]); return 2; }
            static uint8_t buffer[1 << 16];
            const size_t size = fread(buffer, 1, sizeof(buffer), f);
            fclose(f);
            LLVMFuzzerTestOneInput(buffer, size);
        }

        return 0;
    }

    state = (argc > 2) ? strtoull(argv[2], nullptr, 10) | 1 : 0x9E3779B97F4A7C15ULL;

    static uint8_t inputs[2][1024];
    size_t sizes[2] = {0, 0};
    unsigned current = 0;
    unsigned long long executions = 0;

    const time_t stop = time(nullptr) + seconds;

    while (time(nullptr) < stop)
    {
        for (int batch = 0; batch < 256; batch++)
        {
            const unsigned other = current ^ 1;
            sizes[current] = generate(inputs[current], sizeof(inputs[current]), inputs[other], sizes[other]);
            save(inputs[current], sizes[current]);
            LLVMFuzzerTestOneInput(inputs[current], sizes[current]);
            executions++;
            current = other;
        }
    }

    printf("stat::number_of_executed_units: %llu\n", executions);
    return 0;
}
