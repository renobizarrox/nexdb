CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -fno-inline -Wall -Wextra -Wshadow -Iinclude
LDFLAGS ?= -lm -lpthread

# -Wno-format-truncation is a GCC-only flag. Deliberate snprintf truncation into
# fixed-size name and error buffers is all over the parser, and GCC complains
# about every one of them. Clang has no such warning, and would only grumble
# about being handed a flag it does not recognise, so only add it for GCC.
IS_CLANG := $(shell $(CC) --version 2>&1 | grep -ci clang)
ifeq ($(IS_CLANG),0)
  CFLAGS += -Wno-format-truncation
endif

# Optional TLS support — enables --tls-cert / --tls-key in serve mode.
# Tries pkg-config first, then Homebrew, then gives up (builds without TLS).
TLS_CFLAGS :=
TLS_LDFLAGS :=
ifdef NEXDB_TLS
  ifneq ($(NEXDB_TLS),0)
    TLS_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
    TLS_LDFLAGS := $(shell pkg-config --libs openssl 2>/dev/null)
    ifeq ($(TLS_CFLAGS),)
      TLS_CFLAGS := -I/opt/homebrew/opt/openssl@3/include
      TLS_LDFLAGS := -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto
    endif
    TLS_CFLAGS += -DENABLE_TLS
    $(info TLS support enabled)
  endif
endif
CFLAGS += $(TLS_CFLAGS)
LDFLAGS += $(TLS_LDFLAGS)

SRC := src/value.c src/pager.c src/memory.c src/btree.c src/lexer.c \
       src/parser.c src/func.c src/select.c src/exec.c src/main.c src/wal.c src/server.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/nexdb

.PHONY: all clean test demo run asan

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

build/%.o: src/%.c include/nexdb.h include/server.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

# the full integration suite: 137 checks, each in a fresh process
test: $(BIN)
	@sh tests/run_tests.sh

# the guided tour; safe to run repeatedly
demo: $(BIN)
	@rm -f demo.ndb
	@./$(BIN) demo.ndb -f examples/demo.sql

# interactive shell on a database called brain.ndb
run: $(BIN)
	@./$(BIN) brain.ndb

# same tests under AddressSanitizer and UndefinedBehaviorSanitizer
asan:
	@mkdir -p build
	$(CC) -std=c11 -g -O1 -fno-inline -fsanitize=address,undefined -Iinclude \
	      $(TLS_CFLAGS) $(SRC) -o build/nexdb-asan $(LDFLAGS)
	@BIN=./build/nexdb-asan sh tests/run_tests.sh

clean:
	rm -rf build *.ndb
