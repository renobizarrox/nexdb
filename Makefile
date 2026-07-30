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
	      $(SRC) -o build/nexdb-asan $(LDFLAGS)
	@BIN=./build/nexdb-asan sh tests/run_tests.sh

clean:
	rm -rf build *.ndb
