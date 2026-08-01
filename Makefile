CC      = gcc
STD     = -std=c11
WARN    = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
OPT     = -O2
DEBUG   = -g3 -fsanitize=address,undefined
DEFINES = -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE

CFLAGS_RELEASE = $(STD) $(WARN) $(OPT) $(DEFINES)
CFLAGS_DEBUG   = $(STD) $(WARN) $(OPT) $(DEBUG) $(DEFINES)

CFLAGS ?= $(CFLAGS_RELEASE)

LDFLAGS =
LDLIBS  = -lm

SRC_DIR   = src
INC_DIR   = include
OBJ_DIR   = build/obj
BIN_DIR   = build/bin
TEST_DIR  = tests
TOOLS_DIR = tools

LIB_SRCS = \
	$(SRC_DIR)/types.c    \
	$(SRC_DIR)/error.c    \
	$(SRC_DIR)/record.c   \
	$(SRC_DIR)/lock.c     \
	$(SRC_DIR)/storage.c  \
	$(SRC_DIR)/index.c    \
	$(SRC_DIR)/query.c    \
	$(SRC_DIR)/table.c    \
	$(SRC_DIR)/kumdb.c    \
	$(SRC_DIR)/sql.c

LIB_OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(LIB_SRCS))

STATIC_LIB = build/libkumdb.a

TOOLS = \
	$(BIN_DIR)/kumdb_cli \
	$(BIN_DIR)/bench     \
	$(BIN_DIR)/dump

TESTS = \
	$(BIN_DIR)/test_core    \
	$(BIN_DIR)/test_query   \
	$(BIN_DIR)/test_types   \
	$(BIN_DIR)/test_storage \
	$(BIN_DIR)/test_sql

.PHONY: all release debug lib tools tests clean distclean help

all: release

release: CFLAGS = $(CFLAGS_RELEASE)
release: lib tools

debug: CFLAGS = $(CFLAGS_DEBUG)
debug: lib tools tests

lib: $(STATIC_LIB)

tools: $(TOOLS)

tests: $(TESTS)

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(BIN_DIR)/kumdb_cli: $(TOOLS_DIR)/kumdb_cli.c $(STATIC_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BIN_DIR)/bench: $(TOOLS_DIR)/bench.c $(STATIC_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BIN_DIR)/dump: $(TOOLS_DIR)/dump.c $(STATIC_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BIN_DIR)/test_core: $(TEST_DIR)/test_core.c $(STATIC_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BIN_DIR)/test_query: $(TEST_DIR)/test_query.c $(STATIC_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BIN_DIR)/test_types: $(TEST_DIR)/test_types.c $(STATIC_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BIN_DIR)/test_storage: $(TEST_DIR)/test_storage.c $(STATIC_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BIN_DIR)/test_sql: $(TEST_DIR)/test_sql.c $(STATIC_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) $< $(STATIC_LIB) $(LDLIBS) -o $@

.PHONY: check
check: tests
	@echo "Running tests..."
	@failed=0; \
	for t in $(TESTS); do \
		if [ -f $$t ]; then \
			printf "  %-30s " $$(basename $$t); \
			if $$t > /dev/null 2>&1; then \
				echo "PASS"; \
			else \
				echo "FAIL"; \
				failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	if [ $$failed -eq 0 ]; then \
		echo "All tests passed."; \
	else \
		echo "$$failed test(s) failed."; \
		exit 1; \
	fi

clean:
	rm -rf build

distclean: clean
	find . -name "*.kdb" -delete
	find . -name "*.kdb.tmp" -delete
	find . -name "*.kdb.lock" -delete

help:
	@echo "KumDB build targets:"
	@echo "  make             - build release lib + tools"
	@echo "  make debug       - build with ASAN + debug symbols"
	@echo "  make lib         - build libkumdb.a only"
	@echo "  make tools       - build CLI tools"
	@echo "  make tests       - build test binaries"
	@echo "  make check       - build and run all tests"
	@echo "  make clean       - remove build artifacts"
	@echo "  make distclean   - clean + remove .kdb data files"