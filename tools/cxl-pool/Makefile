# SPDX-License-Identifier: GPL-2.0
CC ?= cc
CPPFLAGS += -idirafter ../../linux/include/uapi
CFLAGS += -std=gnu11 -O2 -Wall -Wextra -Werror
BUILD_DIR := ../../build/tools/cxl-pool
TOOLS := $(addprefix $(BUILD_DIR)/,pcreg_cli pool_bloom_tool)

.PHONY: all clean
all: $(TOOLS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/pcreg_cli: pcreg_cli.c ../../linux/include/uapi/linux/cxl_pagecache_reg.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

$(BUILD_DIR)/pool_bloom_tool: pool_bloom_tool.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

clean:
	$(RM) $(TOOLS)
