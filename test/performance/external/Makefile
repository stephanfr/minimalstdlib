# Build rules for external MPSC queue reference implementations used in
# the MpScGrowableRingQueueExternalComparisonPerformanceTests group.
#
# Included by Makefile.test.mk from the project root.  The variables
# PERFORMANCE_OBJ_DIR, CC, CPP_FLAGS, TEST_OPTIMIZATION_FLAGS,
# TEST_CPP_FLAGS, DEPFLAGS, GCC_NATIVE_TRIPLE, and GCC_NATIVE_VERSION
# are all inherited from that context.

EXTERNAL_QUEUE_DIR := test/performance/external/mp_sc_growable_ring_queue

# ---------------------------------------------------------------------------
# moodycamel ConcurrentQueue runner
# Compiled without -Iinclude so #include <atomic> etc. resolve to the real
# standard library rather than the minstd overlay headers.
# ---------------------------------------------------------------------------
MOODYCAMEL_SRC := $(EXTERNAL_QUEUE_DIR)/test_runner.cpp
MOODYCAMEL_INCLUDE_DIRS := \
	-I. \
	-I$(EXTERNAL_QUEUE_DIR) \
	-I/usr/include \
	-I/usr/lib/gcc/$(GCC_NATIVE_TRIPLE)/$(GCC_NATIVE_VERSION)/include \
	-I/usr/include/$(GCC_NATIVE_TRIPLE) \
	-I/usr/include/c++/$(GCC_NATIVE_VERSION) \
	-I/usr/include/$(GCC_NATIVE_TRIPLE)/c++/$(GCC_NATIVE_VERSION)

PERFORMANCE_OBJ += $(PERFORMANCE_OBJ_DIR)/moodycamel_runner.o

$(PERFORMANCE_OBJ_DIR)/moodycamel_runner.o: $(MOODYCAMEL_SRC)
	@/bin/mkdir -p $(PERFORMANCE_OBJ_DIR)
	$(CC) $(MOODYCAMEL_INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(DEPFLAGS) -c $< -o $@
