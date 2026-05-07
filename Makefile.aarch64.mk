# Copyright 2023 Stephan Friedl. All rights reserved.
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

include Makefile.toolchain.aarch64.mk

MINIMALCLIB_DIR ?= ../minimalclib

CPP_SRC_DIR := src

OBJ_DIR := build/aarch64
LIB_DIR := lib/aarch64

LIB := $(LIB_DIR)/libminimalstdlib.a
CPP_SRC := $(wildcard $(CPP_SRC_DIR)/*.cpp)
CPP_OBJ := $(CPP_SRC:$(CPP_SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
CPP_DEP := $(CPP_OBJ:.o=.d)
DEPFLAGS := -MMD -MP

C_DEFINES := 
INCLUDE_DIRS := -I$(MINIMALCLIB_DIR)/include -Iinclude $(INCLUDE_DIRS)


$(LIB) : $(CPP_OBJ)
	$(AR) rcs $(LIB) $(CPP_OBJ)

$(OBJ_DIR)/%.o: $(CPP_SRC_DIR)/%.cpp
	@/bin/mkdir -p $(OBJ_DIR) $(LIB_DIR)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(OPTIMIZATION_FLAGS) $(DEPFLAGS) -c $< -o $@

-include $(CPP_DEP)


lib: $(LIB)

clean:
	/bin/rm $(LIB_DIR)/*.* $(OBJ_DIR)/*.o $(OBJ_DIR)/*.d > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(OBJ_DIR) > /dev/null 2> /dev/null || true
	/bin/mkdir -p $(LIB_DIR) > /dev/null 2> /dev/null || true

