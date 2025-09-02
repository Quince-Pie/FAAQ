ifneq ($(filter /bin/sh,$(SHELL)),)
    # POSIX sh: -e (exit on error)
    .SHELLFLAGS := -ec
else
    # Bash: -e (exit on error), -u (unset variables error), -o pipefail
    .SHELLFLAGS := -euo pipefail -c
endif

# Execute all lines of a recipe in a single shell invocation.
.ONESHELL:
# Delete targets if their recipe fails.
.DELETE_ON_ERROR:
# Clear built-in suffix rules.
.SUFFIXES:
.DEFAULT_GOAL := all


CC := gcc
AR := ar
RM := rm -rf
CLANG_FORMAT := clang-format

BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
LIB_DIR   := $(BUILD_DIR)/lib
BIN_DIR   := $(BUILD_DIR)/bin

# ------------------------------------------------------------------------------
# Flags Management
# ------------------------------------------------------------------------------

# C Standard, Warnings, Optimization, Debugging, and PIC
CFLAGS := -std=c23 -Wall -Wextra -Wpedantic -O2 -g -fPIC

# Automatic Dependency Generation
CFLAGS += -MMD -MP

LDFLAGS :=

# Check if the compiler supports -fhardened by inspecting its help output.
SUPPORTS_FHARDENED := $(shell $(CC) --help=hardened 2>&1 | grep -q "The following options are enabled by -fhardened" && echo 1)

ifeq ($(SUPPORTS_FHARDENED),1)
  CFLAGS += -fhardened
  # Explicitly ensure critical linker hardening flags are present.
  LDFLAGS += -Wl,-z,now -Wl,-z,relro
  $(info INFO: GCC Hardening (-fhardened) enabled.)
else
  $(warning WARNING: Compiler $(CC) does not support -fhardened.)
endif

HP_SRCS   := hp.c
FAAQ_SRCS := faaq.c

TEST_SRCS    := hp_test.c faaq_hp_test.c
BENCH_SRCS   := faaq_bench.c
EXAMPLE_SRCS := example.c

FORMAT_FILES := $(wildcard *.c) $(wildcard *.h)

# Map sources to objects (using substitution references)
HP_OBJS      := $(HP_SRCS:%.c=$(OBJ_DIR)/%.o)
FAAQ_OBJS    := $(FAAQ_SRCS:%.c=$(OBJ_DIR)/%.o)
TEST_OBJS    := $(TEST_SRCS:%.c=$(OBJ_DIR)/%.o)
BENCH_OBJS   := $(BENCH_SRCS:%.c=$(OBJ_DIR)/%.o)
EXAMPLE_OBJS := $(EXAMPLE_SRCS:%.c=$(OBJ_DIR)/%.o)

# Library Targets
LIBHP_A   := $(LIB_DIR)/libhp.a
LIBHP_SO  := $(LIB_DIR)/libhp.so
LIBFAAQ_A := $(LIB_DIR)/libfaaq.a
LIBFAAQ_SO:= $(LIB_DIR)/libfaaq.so
LIBS := $(LIBHP_A) $(LIBHP_SO) $(LIBFAAQ_A) $(LIBFAAQ_SO)

# Executable Targets
TEST_BINS    := $(TEST_SRCS:%.c=$(BIN_DIR)/%)
BENCH_BINS   := $(BENCH_SRCS:%.c=$(BIN_DIR)/%)
EXAMPLE_BINS := $(EXAMPLE_SRCS:%.c=$(BIN_DIR)/%)
BINS := $(TEST_BINS) $(BENCH_BINS) $(EXAMPLE_BINS)

# Collect all objects and dependency files
ALL_OBJS := $(HP_OBJS) $(FAAQ_OBJS) $(TEST_OBJS) $(BENCH_OBJS) $(EXAMPLE_OBJS)
DEPS     := $(ALL_OBJS:.o=.d)

define PRINT =
  $(let TYPE,$1,$(let TGT,$2,$(info [$(TYPE)] $(TGT))))
endef

.PHONY: all libs bins test bench clean dist format check-format

all: libs .WAIT bins

libs: $(LIBS)
bins: $(BINS)

# Use .NOTINTERMEDIATE (GNU Make 4.4) to ensure libraries are preserved.
.NOTINTERMEDIATE: $(LIBS)

# --- Directory Preparation ---

# Ensure directories exist. Use order-only prerequisites (|) so that directory
# timestamp changes do not force rebuilds of the targets.
$(BUILD_DIR) $(OBJ_DIR) $(LIB_DIR) $(BIN_DIR):
	@mkdir -p $@

# --- Compilation Pattern Rule ---

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(call PRINT,CC,$<)
	$(CC) $(CFLAGS) -c $< -o $@

# --- Library Rules ---

# Static Libraries
# Strategy: Bundle hp.o into libfaaq.a as faaq.c depends on hp.c.

$(LIBHP_A): $(HP_OBJS) | $(LIB_DIR)
	$(call PRINT,AR,$@)
	$(AR) rcs $@ $^

$(LIBFAAQ_A): $(FAAQ_OBJS) $(HP_OBJS) | $(LIB_DIR)
	$(call PRINT,AR,$@)
	$(AR) rcs $@ $^

# Dynamic Libraries
# Strategy: libfaaq.so links dynamically against libhp.so.

$(LIBHP_SO): $(HP_OBJS) | $(LIB_DIR)
	$(call PRINT,LDSO,$@)
	$(CC) $(LDFLAGS) -shared -o $@ $^

$(LIBFAAQ_SO): $(FAAQ_OBJS) $(LIBHP_SO) | $(LIB_DIR)
	$(call PRINT,LDSO,$@)
	# Link FAAQ objects and specify dependency on libhp
	$(CC) $(LDFLAGS) -shared -o $@ $(FAAQ_OBJS) -L$(LIB_DIR) -lhp

# --- Executable Rules ---

# Strategy: Link executables dynamically for development convenience.
# Set RPATH so the binaries can find the shared libraries in the build directory.
RPATH_FLAG := -Wl,-rpath,$(abspath $(LIB_DIR))

# Helper macro for linking against FAAQ (which requires HP)
define LINK_DYN =
	$(call PRINT,LD,$@)
	# $1: Object files, $2: Libraries (e.g., -lfaaq -lhp)
	$(CC) $(LDFLAGS) $(1) -o $@ -L$(LIB_DIR) $(2) $(RPATH_FLAG)
endef

$(BIN_DIR)/hp_test: $(OBJ_DIR)/hp_test.o $(LIBHP_SO) | $(BIN_DIR)
	$(call LINK_DYN,$(OBJ_DIR)/hp_test.o,-lhp)

$(BIN_DIR)/faaq_hp_test: $(OBJ_DIR)/faaq_hp_test.o $(LIBFAAQ_SO) $(LIBHP_SO) | $(BIN_DIR)
	$(call LINK_DYN,$(OBJ_DIR)/faaq_hp_test.o,-lfaaq -lhp)

$(BIN_DIR)/faaq_bench: $(OBJ_DIR)/faaq_bench.o $(LIBFAAQ_SO) $(LIBHP_SO) | $(BIN_DIR)
	$(call LINK_DYN,$(OBJ_DIR)/faaq_bench.o,-lfaaq -lhp)

$(BIN_DIR)/example: $(OBJ_DIR)/example.o $(LIBFAAQ_SO) $(LIBHP_SO) | $(BIN_DIR)
	$(call LINK_DYN,$(OBJ_DIR)/example.o,-lfaaq -lhp)


test: $(TEST_BINS)
	$(call PRINT,RUN,Tests)
	# The strict shell mode (-e) combined with .ONESHELL ensures we stop immediately if a test fails.
	for t in $(TEST_BINS); do
	    echo "-> Executing $$t"
	    ./$$t
	done
	@echo "-> All tests passed."

bench: $(BENCH_BINS)
	$(call PRINT,RUN,Benchmarks)
	for b in $(BENCH_BINS); do
	    echo "-> Executing $$b"
	    ./$$b
	done

format:
	$(call PRINT,FORMAT,Source files)
	# -i: Edit files in-place
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

check-format:
	$(call PRINT,CHECK,Code style)
	# --dry-run: Print warnings for bad formatting without changing files.
	# -Werror: Treat those warnings as errors and exit non-zero (for CI).
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_FILES)
	@echo "-> Code style compliant."

# Distribution Target (using zstd)
PROJECT     := faaq
VERSION     := 1.0.0
DIST_NAME   := $(PROJECT)-$(VERSION)
DIST_FILE   := $(DIST_NAME).tar.zst
# Explicitly define files to include in the distribution
DIST_FILES  := Makefile $(FORMAT_FILES)

dist: $(DIST_FILE)

$(DIST_FILE): $(DIST_FILES)
	$(call PRINT,DIST,$@)
	# Use --transform to place files inside the $(DIST_NAME) directory in the archive
	tar --zstd -cf $@ --transform="s,^,$(DIST_NAME)/," $(DIST_FILES)

clean:
	$(call PRINT,CLEAN,Build artifacts)
	$(RM) $(BUILD_DIR)
	$(RM) $(DIST_FILE)

# Include the dependency files generated by GCC.
# The '-' prefix ignores errors if the files don't exist yet.
-include $(DEPS)
