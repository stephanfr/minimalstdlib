// C-linkage interface to the moodycamel runner TU.
// The runner is compiled without -Iinclude so it uses real std::atomic etc.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

double run_moodycamel(size_t num_producers, size_t total_items);

#ifdef __cplusplus
}
#endif
