# LDOS Tiered Memory Manager - Makefile
#
# Build targets:
#   make             - Build everything
#   make lib         - Build only the shim library
#   make demo        - Build only the demo program
#   make clean       - Remove build artifacts
#   make debug       - Build with debug symbols and no optimization
#
# Usage:
#   1. Build: make
#   2. Run demo: ./bin/tiered_manager
#   3. Use shim: LD_PRELOAD=./lib/libmmap_shim.so ./your_program

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -fPIC -Isrc
LDFLAGS = -pthread -ldl -lm

# Debug vs Release
DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2 -DNDEBUG
endif

# Directories
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
LIB_DIR = lib

# Source files
ALL_SRCS = $(wildcard $(SRC_DIR)/*.c)

# Core sources (everything except main.c and mmap_shim.c)
CORE_SRCS = $(filter-out $(SRC_DIR)/main.c $(SRC_DIR)/mmap_shim.c, $(ALL_SRCS))

# Shim-specific source
SHIM_SRC = $(SRC_DIR)/mmap_shim.c

# Demo-specific source
DEMO_SRC = $(SRC_DIR)/main.c

# Object files
CORE_OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))
SHIM_OBJ  = $(OBJ_DIR)/mmap_shim.o
DEMO_OBJ  = $(OBJ_DIR)/main.o

# Headers (for dependency tracking)
HEADERS = $(wildcard $(SRC_DIR)/*.h)

# Output files
SHIM_LIB = $(LIB_DIR)/libmmap_shim.so
DEMO_BIN = $(BIN_DIR)/tiered_manager

# Default target
all: dirs $(SHIM_LIB) $(DEMO_BIN)

# Ensure directories exist
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)

# Shim library (for LD_PRELOAD)
lib: dirs $(SHIM_LIB)

$(SHIM_LIB): $(CORE_OBJS) $(SHIM_OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS)
	@echo "Built $@ - use with: LD_PRELOAD=./$@ ./your_program"

# Demo program
demo: dirs $(DEMO_BIN)

$(DEMO_BIN): $(CORE_OBJS) $(DEMO_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Built $@ - run with: ./$@"

# Object file compilation (all sources use the same rule)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Debug build
debug:
	$(MAKE) DEBUG=1

# Clean
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)
	@echo "Cleaned build artifacts"

# Install (to /usr/local by default)
PREFIX ?= /usr/local
install: $(SHIM_LIB)
	install -d $(PREFIX)/lib
	install -m 644 $(SHIM_LIB) $(PREFIX)/lib/
	install -d $(PREFIX)/include
	install -m 644 $(SRC_DIR)/tiered_memory.h $(PREFIX)/include/
	@echo "Installed to $(PREFIX)"

# Uninstall
uninstall:
	rm -f $(PREFIX)/lib/libmmap_shim.so
	rm -f $(PREFIX)/include/tiered_memory.h
	@echo "Uninstalled from $(PREFIX)"

# Help
help:
	@echo "LDOS Tiered Memory Manager - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all (default) - Build everything"
	@echo "  lib           - Build only libmmap_shim.so"
	@echo "  demo          - Build only tiered_manager"
	@echo "  debug         - Build with debug flags"
	@echo "  clean         - Remove build artifacts"
	@echo "  install       - Install to PREFIX (default: /usr/local)"
	@echo "  help          - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  DEBUG=1       - Enable debug build"
	@echo "  PREFIX=path   - Installation prefix"
	@echo ""
	@echo "Examples:"
	@echo "  make                 # Build everything"
	@echo "  make DEBUG=1         # Debug build"
	@echo "  ./bin/tiered_manager # Run demo"
	@echo "  LD_PRELOAD=./lib/libmmap_shim.so ./app  # Use shim"

.PHONY: all dirs lib demo debug clean install uninstall help
