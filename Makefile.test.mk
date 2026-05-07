# Copyright 2023 Stephan Friedl. All rights reserved.
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

include Makefile.native.mk

# ---------------------------------------------------------------------------
# Source directories
# ---------------------------------------------------------------------------
CORRECTNESS_SRC_DIR := test/correctness
PERFORMANCE_SRC_DIR := test/performance
HOST_PERF_SRC_DIR   := test/performance_host
SOAK_SRC_DIR        := test/soak
SHARED_SRC_DIR      := test/shared

# ---------------------------------------------------------------------------
# Build/output directories
# ---------------------------------------------------------------------------
CORRECTNESS_OBJ_DIR := test/build/correctness
PERFORMANCE_OBJ_DIR := test/build/performance
HOST_PERF_OBJ_DIR   := test/build/performance_host
SOAK_OBJ_DIR        := test/build/soak
LIB_OBJ_DIR         := test/build/lib
COVERAGE_OBJ_DIR    := test/coverage/correctness
COVERAGE_LIB_OBJ_DIR := test/coverage/lib

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
CORRECTNESS_EXE := $(CORRECTNESS_OBJ_DIR)/cpputest_correctness.exe
LOCKFREE_CORRECTNESS_EXE := $(CORRECTNESS_OBJ_DIR)/cpputest_lockfree_correctness.exe
PERFORMANCE_EXE := $(PERFORMANCE_OBJ_DIR)/cpputest_performance.exe
HOST_PERF_EXE   := $(HOST_PERF_OBJ_DIR)/std_containers_perf.exe
SOAK_EXE        := $(SOAK_OBJ_DIR)/cpputest_soak.exe
COVERAGE_EXE    := $(COVERAGE_OBJ_DIR)/cpputest_correctness_coverage.exe

# ---------------------------------------------------------------------------
# Compiler / flag settings
# ---------------------------------------------------------------------------
CDEFINES     := -D__MINIMAL_STD_TEST__
INCLUDE_DIRS := -I. -Iinclude -Itest/shared -I$(CPPUTEST_PATH)/include $(INCLUDE_DIRS)
DEPFLAGS     := -MMD -MP

MINIMALCLIB_DIR ?= ../minimalclib
LDLIBS   := -L$(MINIMALCLIB_DIR)/lib/$(NATIVE_BUILD_DIR) -lminimalclib
TEST_LIB := -L$(CPPUTEST_PATH)/lib -lCppUTest -lCppUTestExt

# ---------------------------------------------------------------------------
# Source file lists
# ---------------------------------------------------------------------------
SHARED_SRC      := $(wildcard $(SHARED_SRC_DIR)/*.cpp)
ALL_CORRECTNESS_SRC := $(wildcard $(CORRECTNESS_SRC_DIR)/*.cpp)
PERFORMANCE_SRC := $(wildcard $(PERFORMANCE_SRC_DIR)/*.cpp)
SOAK_SRC        := $(wildcard $(SOAK_SRC_DIR)/*.cpp)
HOST_PERF_SRC   := $(wildcard $(HOST_PERF_SRC_DIR)/*.cpp)

# Library sources (compiled into every test binary)
LIB_SRC := $(CPP_SRC)

# ---------------------------------------------------------------------------
# Object file lists — each binary gets its own object directory to avoid
# conflicts when the same shared source is compiled with different flags.
# ---------------------------------------------------------------------------
LOCKFREE_CORRECTNESS_SRC := \
	$(CORRECTNESS_SRC_DIR)/lockfree_single_arena_memory_resource_test.cpp \
	$(CORRECTNESS_SRC_DIR)/lockfree_single_arena_memory_resource_multithread_test.cpp
CORRECTNESS_SRC     := $(filter-out $(LOCKFREE_CORRECTNESS_SRC), $(ALL_CORRECTNESS_SRC))

CORRECTNESS_OBJ := $(patsubst $(CORRECTNESS_SRC_DIR)/%.cpp,$(CORRECTNESS_OBJ_DIR)/%.o,\
	                  $(CORRECTNESS_SRC)) \
	               $(patsubst $(SHARED_SRC_DIR)/%.cpp,$(CORRECTNESS_OBJ_DIR)/shared_%.o,\
	                  $(SHARED_SRC))

LOCKFREE_CORRECTNESS_OBJ := $(patsubst $(CORRECTNESS_SRC_DIR)/%.cpp,$(CORRECTNESS_OBJ_DIR)/%.o,\
	                          $(LOCKFREE_CORRECTNESS_SRC)) \
	                        $(CORRECTNESS_OBJ_DIR)/cpputest_correctness_main.o \
	                        $(patsubst $(SHARED_SRC_DIR)/%.cpp,$(CORRECTNESS_OBJ_DIR)/shared_%.o,$(SHARED_SRC))
PERFORMANCE_OBJ := $(patsubst $(PERFORMANCE_SRC_DIR)/%.cpp,$(PERFORMANCE_OBJ_DIR)/%.o,\
	                  $(PERFORMANCE_SRC)) \
	               $(patsubst $(SHARED_SRC_DIR)/%.cpp,$(PERFORMANCE_OBJ_DIR)/shared_%.o,\
	                  $(SHARED_SRC))
SOAK_OBJ        := $(patsubst $(SOAK_SRC_DIR)/%.cpp,$(SOAK_OBJ_DIR)/%.o,\
	                  $(SOAK_SRC)) \
	               $(patsubst $(SHARED_SRC_DIR)/%.cpp,$(SOAK_OBJ_DIR)/shared_%.o,\
	                  $(SHARED_SRC))
HOST_PERF_OBJ   := $(patsubst $(HOST_PERF_SRC_DIR)/%.cpp,$(HOST_PERF_OBJ_DIR)/%.o,$(HOST_PERF_SRC))

LIB_OBJ         := $(patsubst $(CPP_SRC_DIR)/%.cpp,$(LIB_OBJ_DIR)/%.o,$(LIB_SRC))

COVERAGE_OBJ    := $(patsubst $(CORRECTNESS_SRC_DIR)/%.cpp,$(COVERAGE_OBJ_DIR)/%.o,\
	                  $(ALL_CORRECTNESS_SRC)) \
	               $(patsubst $(SHARED_SRC_DIR)/%.cpp,$(COVERAGE_OBJ_DIR)/shared_%.o,\
	                  $(SHARED_SRC))
COVERAGE_LIB_OBJ := $(patsubst $(CPP_SRC_DIR)/%.cpp,$(COVERAGE_LIB_OBJ_DIR)/%.o,$(LIB_SRC))

DEP_FILES := $(CORRECTNESS_OBJ:.o=.d) \
	$(LOCKFREE_CORRECTNESS_OBJ:.o=.d) \
	$(PERFORMANCE_OBJ:.o=.d) \
	$(SOAK_OBJ:.o=.d) \
	$(HOST_PERF_OBJ:.o=.d) \
	$(LIB_OBJ:.o=.d) \
	$(COVERAGE_OBJ:.o=.d) \
	$(COVERAGE_LIB_OBJ:.o=.d)

# ---------------------------------------------------------------------------
# Phony targets
# ---------------------------------------------------------------------------
.PHONY: test test-correctness test-performance test-performance-host-std test-soak test-coverage clean_test

# ---------------------------------------------------------------------------
# Aggregate targets
# ---------------------------------------------------------------------------
test: test-correctness test-performance test-soak

test-correctness: lib $(CORRECTNESS_EXE) $(LOCKFREE_CORRECTNESS_EXE)
	./$(CORRECTNESS_EXE)
	./$(LOCKFREE_CORRECTNESS_EXE)

test-performance: lib $(PERFORMANCE_EXE)
	./$(PERFORMANCE_EXE)

test-performance-host-std: $(HOST_PERF_EXE)
	./$(HOST_PERF_EXE)

test-soak: lib $(SOAK_EXE)
	./$(SOAK_EXE)

test-coverage:
	$(MAKE) -f Makefile.test.mk clean_test
	$(MAKE) -f Makefile.test.mk $(COVERAGE_EXE)
	cd $(COVERAGE_OBJ_DIR) && \
	gcov ../../$(CORRECTNESS_SRC_DIR)/cpputest_correctness_main.cpp --object-directory . -o /tmp > /dev/null ; \
	lcov --capture --directory . --output-file /tmp/test_coverage.info --ignore-errors mismatch,negative ; \
	lcov --remove /tmp/test_coverage.info '/usr/include/*' '$(CPPUTEST_PATH)/*' \
	     --output-file /tmp/test_coverage_filtered.info --ignore-errors unused ; \
	genhtml /tmp/test_coverage_filtered.info --output-directory coverage_report ; \
	rm -f /tmp/test_coverage.info /tmp/test_coverage_filtered.info /tmp/*.gcov
	find $(COVERAGE_OBJ_DIR) $(COVERAGE_LIB_OBJ_DIR) -name "*.o" -o -name "*.gcno" -o -name "*.gcda" | xargs rm -f
	rm -f $(COVERAGE_EXE)
	rm -f *.gcov *##*.gcov

# ---------------------------------------------------------------------------
# Link rules
# ---------------------------------------------------------------------------
$(CORRECTNESS_EXE): $(LIB_OBJ) $(CORRECTNESS_OBJ)
	$(LD) $(TEST_LDFLAGS) $(LIB_OBJ) $(CORRECTNESS_OBJ) $(LDLIBS) $(TEST_LIB) -o $@

$(LOCKFREE_CORRECTNESS_EXE): $(LIB_OBJ) $(LOCKFREE_CORRECTNESS_OBJ)
	$(LD) $(TEST_LDFLAGS) $(LIB_OBJ) $(LOCKFREE_CORRECTNESS_OBJ) $(LDLIBS) $(TEST_LIB) -o $@

$(PERFORMANCE_EXE): $(LIB_OBJ) $(PERFORMANCE_OBJ)
	$(LD) $(TEST_LDFLAGS) $(LIB_OBJ) $(PERFORMANCE_OBJ) $(LDLIBS) $(TEST_LIB) -o $@

$(SOAK_EXE): $(LIB_OBJ) $(SOAK_OBJ)
	$(LD) $(TEST_LDFLAGS) $(LIB_OBJ) $(SOAK_OBJ) $(LDLIBS) $(TEST_LIB) -o $@

$(HOST_PERF_EXE): $(HOST_PERF_OBJ)
	$(LD) $(TEST_LDFLAGS) $(HOST_PERF_OBJ) -lpthread -o $@

$(COVERAGE_EXE): $(COVERAGE_LIB_OBJ) $(COVERAGE_OBJ)
	$(LD) $(COVERAGE_LIB_OBJ) $(COVERAGE_OBJ) $(LDLIBS) $(TEST_LIB) -lgcov -o $@
	./$(COVERAGE_EXE)

# ---------------------------------------------------------------------------
# Compile rules — correctness
# ---------------------------------------------------------------------------
$(CORRECTNESS_OBJ_DIR)/%.o: $(CORRECTNESS_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(CORRECTNESS_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

$(CORRECTNESS_OBJ_DIR)/shared_%.o: $(SHARED_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(CORRECTNESS_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Compile rules — performance
# ---------------------------------------------------------------------------
$(PERFORMANCE_OBJ_DIR)/%.o: $(PERFORMANCE_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(PERFORMANCE_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

$(PERFORMANCE_OBJ_DIR)/shared_%.o: $(SHARED_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(PERFORMANCE_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Compile rules — soak
# ---------------------------------------------------------------------------
$(SOAK_OBJ_DIR)/%.o: $(SOAK_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(SOAK_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

$(SOAK_OBJ_DIR)/shared_%.o: $(SHARED_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(SOAK_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Compile rules — host-only std performance
# Avoid -Iinclude and -I../minimalclib/include to keep host STL independent
# from minimalstdlib header overlays.
# ---------------------------------------------------------------------------
$(HOST_PERF_OBJ_DIR)/%.o: $(HOST_PERF_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(HOST_PERF_OBJ_DIR)
	$(CC) -I. $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(DEPFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Compile rules — library (shared across all test binaries)
# ---------------------------------------------------------------------------
$(LIB_OBJ_DIR)/%.o: $(CPP_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(LIB_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(TEST_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Compile rules — coverage (correctness only)
# ---------------------------------------------------------------------------
$(COVERAGE_OBJ_DIR)/%.o: $(CORRECTNESS_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(COVERAGE_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(COVERAGE_OPTIMIZATION_FLAGS) $(COVERAGE_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

$(COVERAGE_OBJ_DIR)/shared_%.o: $(SHARED_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(COVERAGE_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(COVERAGE_OPTIMIZATION_FLAGS) $(COVERAGE_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

$(COVERAGE_LIB_OBJ_DIR)/%.o: $(CPP_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(COVERAGE_LIB_OBJ_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(COVERAGE_OPTIMIZATION_FLAGS) $(COVERAGE_CPP_FLAGS) $(CDEFINES) $(DEPFLAGS) -c $< -o $@

-include $(DEP_FILES)

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
clean_test:
	/bin/rm -rf test/build test/coverage > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(CORRECTNESS_OBJ_DIR) > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(PERFORMANCE_OBJ_DIR) > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(HOST_PERF_OBJ_DIR) > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(SOAK_OBJ_DIR) > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(LIB_OBJ_DIR) > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(COVERAGE_OBJ_DIR) > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(COVERAGE_LIB_OBJ_DIR) > /dev/null 2> /dev/null || true
	/bin/mkdir -p build/x64 > /dev/null 2> /dev/null || true
	/bin/mkdir -p lib/x64 > /dev/null 2> /dev/null || true
	/bin/mkdir -p test/performance/reports > /dev/null 2> /dev/null || true

echo:
	@echo "Correctness sources:  " $(CORRECTNESS_SRC)
	@echo "Performance sources:  " $(PERFORMANCE_SRC)
	@echo "Soak sources:         " $(SOAK_SRC)
	@echo "Library sources:      " $(LIB_SRC)

