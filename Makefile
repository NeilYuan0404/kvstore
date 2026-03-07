# ============================================================================
#  Project: KVStore
#  Description: Lightweight key-value store with hash table and reactor
#  Author: Neil Yuan
# ============================================================================

# Compiler and tools
CC       = gcc
AR       = ar
RM       = rm -f
MKDIR    = mkdir -p

# Directories
SRCDIR   = src
INCDIR   = include
OBJDIR   = obj
BINDIR   = bin
TESTDIR  = test
TESTBINDIR = $(BINDIR)/tests

# Target executable
TARGET   = $(BINDIR)/kvstore

# Source files (automatically list all .c files in SRCDIR)
SOURCES  = $(wildcard $(SRCDIR)/*.c)
OBJECTS  = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Compiler flags
CFLAGS   = -I$(INCDIR) -Wall -Wextra -g -O2 -D_GNU_SOURCE
LDFLAGS  = -L/usr/lib/x86_64-linux-gnu   # optional, if jemalloc is not in default path
LDLIBS   = -lpthread -ldl -ljemalloc

# Debug flags (use `make DEBUG=1` to enable)
ifeq ($(DEBUG),1)
    CFLAGS += -DDEBUG -O0 -g3 -ggdb -fsanitize=address
    LDFLAGS += -fsanitize=address
else
    CFLAGS += -DNDEBUG
endif

# ============================================================================
#  Main target
# ============================================================================
all: $(TARGET)

# Link the final executable
$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Compile source files into objects
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create output directories
$(OBJDIR) $(BINDIR) $(TESTBINDIR):
	$(MKDIR) $@

# ============================================================================
#  Test targets - support multiple test cases
# ============================================================================

# List all test source files (all .c files in test directory)
TEST_SOURCES = $(wildcard $(TESTDIR)/*.c)
# Generate test executable names (test_*.c -> bin/tests/test_*)
TEST_TARGETS = $(TEST_SOURCES:$(TESTDIR)/%.c=$(TESTBINDIR)/%)

# Objects excluding main (kvstore.o contains main)
MAIN_OBJ    = $(OBJDIR)/kvstore.o
CORE_OBJS   = $(filter-out $(MAIN_OBJ), $(OBJECTS))

# Test-specific kvstore object (without main)
KVSTORE_TEST_OBJ = $(OBJDIR)/kvstore_test.o

# Compile test-specific kvstore object (with -DTEST_MODE)
$(KVSTORE_TEST_OBJ): $(SRCDIR)/kvstore.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DTEST_MODE -c $< -o $@

# Pattern rule to compile test source files
$(OBJDIR)/test/%.o: $(TESTDIR)/%.c | $(OBJDIR)/test
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

# Create test object directory
$(OBJDIR)/test:
	$(MKDIR) $@

# Pattern rule to link each test executable
$(TESTBINDIR)/%: $(OBJDIR)/test/%.o $(KVSTORE_TEST_OBJ) $(CORE_OBJS) | $(TESTBINDIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Phony target to build all tests
.PHONY: test
test: $(TEST_TARGETS)

# ============================================================================
#  Individual test targets (convenience)
# ============================================================================
.PHONY: test-case

test-case: $(TESTBINDIR)/testcase
test-special: $(TESTBINDIR)/test_special
test-aof: $(TESTBINDIR)/test_aof
test-rdb: $(TESTBINDIR)/test_rdb
test-repl: $(TESTBINDIR)/test_repl

# ============================================================================
#  Run tests (optional)
# ============================================================================
.PHONY: run-tests run-test-case run-test-resp

run-tests: $(TEST_TARGETS)
	@echo "========================================="
	@echo "Running all tests against local server"
	@echo "========================================="
	@for test in $(TEST_TARGETS); do \
		echo "\nRunning $$(basename $$test)"; \
		$$test 127.0.0.1 8888; \
	done

run-test-case: $(TESTBINDIR)/testcase
	$(TESTBINDIR)/testcase 127.0.0.1 8888 1000

run-test-resp: $(TESTBINDIR)/test_resp
	$(TESTBINDIR)/test_resp 127.0.0.1 8888

# ============================================================================
#  Clean up
# ============================================================================
clean:
	$(RM) -r $(OBJDIR) $(BINDIR) $(DEPDIR)

# ============================================================================
#  Dependency generation
# ============================================================================
DEPDIR = .deps
$(shell mkdir -p $(DEPDIR) >/dev/null)
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td
COMPILE.c = $(CC) $(CFLAGS) $(DEPFLAGS) -c
POSTCOMPILE = mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d && touch $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(DEPDIR)/%.d | $(OBJDIR) $(DEPDIR)
	$(COMPILE.c) $< -o $@
	$(POSTCOMPILE)

$(OBJDIR)/test/%.o: $(TESTDIR)/%.c $(DEPDIR)/test/%.d | $(OBJDIR)/test $(DEPDIR)/test
	$(COMPILE.c) -I$(INCDIR) $< -o $@
	$(POSTCOMPILE)

$(DEPDIR)/%.d: ;
$(DEPDIR)/test/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d $(DEPDIR)/test/%.d

# Include dependency files if they exist
-include $(wildcard $(DEPDIR)/*.d)
-include $(wildcard $(DEPDIR)/test/*.d)

# ============================================================================
#  Help
# ============================================================================
help:
	@echo "KVStore Makefile"
	@echo ""
	@echo "Build targets:"
	@echo "  make all           - Build the project (default)"
	@echo "  make clean         - Remove all build files"
	@echo "  make DEBUG=1       - Build with debug symbols and AddressSanitizer"
	@echo ""
	@echo "Test targets:"
	@echo "  make test          - Build ALL test cases"
	@echo "  make test-case     - Build testcase only"
	@echo "  make test-resp     - Build test_resp only"
	@echo "  make test-special  - Build test_special only"
	@echo "  make test-aof      - Build test_aof only"
	@echo "  make test-rdb      - Build test_rdb only"
	@echo "  make test-repl     - Build test_repl only"
	@echo ""
	@echo "Run targets (assumes server running on 127.0.0.1:8888):"
	@echo "  make run-tests     - Run all tests"
	@echo "  make run-test-case - Run testcase with 1000 keys"
	@echo "  make run-test-resp - Run test_resp"
	@echo ""
	@echo "Test files:"
	@echo "  test/testcase.c    - Batch processing benchmark"
	@echo "  test/test_resp.c   - RESP protocol tests"
	@echo "  test/test_special.c - Special character tests"
	@echo "  test/test_aof.c    - AOF persistence tests"
	@echo "  test/test_rdb.c    - RDB persistence tests"
	@echo "  test/test_repl.c   - Replication tests"