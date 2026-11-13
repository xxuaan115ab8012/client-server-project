# Linux System Monitor - Makefile
#
#   make              robust build, DEBUG=0  -> build/release, published to build/bin + build/lib
#   make DEBUG=1      robust build with DEBUG compiled in -> build/debug, published to build/bin + build/lib
#   make baseline     unprotected experiment build -> build/baseline/{bin,lib} (main binaries untouched)
#   make test         run tests/test_protocol.sh and tests/concurrent_10.sh
#   make clean        remove build/
#
# Each mode uses its own object/output directory, so switching DEBUG never
# reuses stale objects (SPEC 10.1 note).  build/bin and build/lib always
# contain a copy of the most recent `make` / `make DEBUG=1` output.

CC      ?= gcc
DEBUG   ?= 0
BASELINE ?= 0

ifeq ($(BASELINE),1)
  MODE       := baseline
  MODE_FLAGS := -O2 -DBASELINE_MODE=1 -DENABLE_DEBUG_LOG=$(DEBUG)
else ifeq ($(DEBUG),1)
  MODE       := debug
  MODE_FLAGS := -g -O0 -DENABLE_DEBUG_LOG=1
else
  MODE       := release
  MODE_FLAGS := -O2 -DENABLE_DEBUG_LOG=0
endif

BUILD    := build
OUT      := $(BUILD)/$(MODE)
OBJDIR   := $(OUT)/obj
BINDIR   := $(OUT)/bin
LIBDIR   := $(OUT)/lib

PUB_BIN  := $(BUILD)/bin
PUB_LIB  := $(BUILD)/lib

CFLAGS   := -std=c11 -Wall -Wextra -Werror -MMD -MP \
            -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
            -Iinclude $(MODE_FLAGS)
LDFLAGS   = -L$(LIBDIR) -Wl,-rpath,'$$ORIGIN/../lib'
LDLIBS   := -lcommon

COMMON_SRC := src/common/net.c src/common/protocol.c src/common/logger.c
SERVER_SRC := src/server/server.c src/server/request_handler.c src/server/system_info.c
CLIENT_SRC := src/client/client.c

COMMON_OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(COMMON_SRC))
SERVER_OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SERVER_SRC))
CLIENT_OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(CLIENT_SRC))

LIBSO    := $(LIBDIR)/libcommon.so
SERVER   := $(BINDIR)/server
CLIENT   := $(BINDIR)/client

.PHONY: all build-mode publish baseline test clean help

# Default: build the current mode and publish to build/bin + build/lib.
all: build-mode publish

build-mode: $(LIBSO) $(SERVER) $(CLIENT)

# Shared library: position independent objects, -shared.
$(COMMON_OBJ): $(OBJDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(LIBSO): $(COMMON_OBJ)
	@mkdir -p $(LIBDIR)
	$(CC) -shared -o $@ $^

# Executables (dynamically linked against libcommon.so)
$(SERVER_OBJ) $(CLIENT_OBJ): $(OBJDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(SERVER): $(SERVER_OBJ) $(LIBSO)
	@mkdir -p $(BINDIR)
	$(CC) $(SERVER_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(CLIENT): $(CLIENT_OBJ) $(LIBSO)
	@mkdir -p $(BINDIR)
	$(CC) $(CLIENT_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

# Copy the current mode's artefacts to the SPEC paths build/bin, build/lib.
publish: build-mode
	@mkdir -p $(PUB_BIN) $(PUB_LIB)
	cp -f $(LIBSO) $(PUB_LIB)/libcommon.so
	cp -f $(SERVER) $(PUB_BIN)/server
	cp -f $(CLIENT) $(PUB_BIN)/client
	@echo "published $(MODE) build -> $(PUB_BIN)/{server,client}, $(PUB_LIB)/libcommon.so"

# Baseline experiment build lives entirely under build/baseline/.
baseline:
	$(MAKE) --no-print-directory BASELINE=1 DEBUG=$(DEBUG) build-mode
	@echo "baseline build -> $(BUILD)/baseline/bin/{server,client}, $(BUILD)/baseline/lib/libcommon.so"

test: all
	bash tests/test_protocol.sh
	bash tests/concurrent_10.sh

clean:
	rm -rf $(BUILD)

help:
	@sed -n '2,12p' Makefile

-include $(COMMON_OBJ:.o=.d) $(SERVER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d)
