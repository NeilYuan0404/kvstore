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

# Target executable
TARGET   = $(BINDIR)/kvstore

# Source files (automatically list all .c files in SRCDIR)
SOURCES  = $(wildcard $(SRCDIR)/*.c)
OBJECTS  = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Compiler flags
CFLAGS   = -I$(INCDIR) -Wall -Wextra -g -O2 -D_GNU_SOURCE
LDFLAGS  = -L/usr/lib/x86_64-linux-gnu   # optional, if jemalloc is not in default path
LDLIBS   =  -lpthread -ldl

# Debug flags (use `make DEBUG=1` to enable)
ifeq ($(DEBUG),1)
    CFLAGS += -DDEBUG -O0 -g3 -ggdb -fsanitize=address
    LDFLAGS += -fsanitize=address
else
    CFLAGS += -DNDEBUG
endif

# Default target
all: $(TARGET)

# Link the final executable
$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Compile source files into objects
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create output directories
$(OBJDIR) $(BINDIR):
	$(MKDIR) $@

# Generate dependency files automatically (optional but recommended)
DEPDIR = .deps
$(shell mkdir -p $(DEPDIR) >/dev/null)
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td
COMPILE.c = $(CC) $(CFLAGS) $(DEPFLAGS) -c
POSTCOMPILE = mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d && touch $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(DEPDIR)/%.d | $(OBJDIR) $(DEPDIR)
	$(COMPILE.c) $< -o $@
	$(POSTCOMPILE)

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d

include $(wildcard $(DEPDIR)/*.d)

# ============================================================================
# Test targets
# ============================================================================
TEST_SRC    = test/testcase.c
TEST_OBJ    = $(OBJDIR)/test/testcase.o
TEST_TARGET = $(BINDIR)/test

# Objects excluding main (kvstore.o contains main)
MAIN_OBJ    = $(OBJDIR)/kvstore.o
CORE_OBJS   = $(filter-out $(MAIN_OBJ), $(OBJECTS))

# Test-specific kvstore object (without main)
KVSTORE_TEST_OBJ = $(OBJDIR)/kvstore_test.o

# Compile test-specific kvstore object (with -DTEST_MODE)
$(KVSTORE_TEST_OBJ): $(SRCDIR)/kvstore.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DTEST_MODE -c $< -o $@

# Compile test source
$(OBJDIR)/test/%.o: test/%.c | $(OBJDIR)/test
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

# Create test object directory
$(OBJDIR)/test:
	$(MKDIR) $@

# Link test executable: test object + kvstore_test + core objects
$(TEST_TARGET): $(TEST_OBJ) $(KVSTORE_TEST_OBJ) $(CORE_OBJS) | $(BINDIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

.PHONY: test
test: $(TEST_TARGET)

# Clean up build artifacts
clean:
	$(RM) -r $(OBJDIR) $(BINDIR) $(DEPDIR)

# Phony targets
.PHONY: all clean help

# Show help (optional)
help:
	@echo "KVStore Makefile"
	@echo "  make all       - Build the project (default)"
	@echo "  make test      - Build test executable (test/testcase.c)"
	@echo "  make clean     - Remove all build files"
	@echo "  make DEBUG=1   - Build with debug symbols, AddressSanitizer, and no optimization"
