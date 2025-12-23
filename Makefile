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
#   2. Run demo: ./tiered_manager
#   3. Use shim: LD_PRELOAD=./libmmap_shim.so ./your_program

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -fPIC
LDFLAGS = -pthread -ldl -lm

# Debug vs Release
DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2 -DNDEBUG
endif

# Source files
CORE_SRCS = tiered_memory.c page_stats.c uffd_handler.c policy_thread.c
SHIM_SRCS = mmap_shim.c
DEMO_SRCS = main.c

# Object files
CORE_OBJS = $(CORE_SRCS:.c=.o)
SHIM_OBJS = $(SHIM_SRCS:.c=.o)
DEMO_OBJS = $(DEMO_SRCS:.c=.o)

# Output files
SHIM_LIB = libmmap_shim.so
DEMO_BIN = tiered_manager

# Default target
all: $(SHIM_LIB) $(DEMO_BIN)

# Shim library (for LD_PRELOAD)
lib: $(SHIM_LIB)

$(SHIM_LIB): $(CORE_OBJS) $(SHIM_OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)
	@echo "Built $@ - use with: LD_PRELOAD=./$@ ./your_program"

# Demo program
demo: $(DEMO_BIN)

$(DEMO_BIN): $(CORE_OBJS) $(DEMO_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Built $@ - run with: ./$@"

# Object file compilation
%.o: %.c tiered_memory.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Debug build
debug:
	$(MAKE) DEBUG=1

# Clean
clean:
	rm -f $(CORE_OBJS) $(SHIM_OBJS) $(DEMO_OBJS)
	rm -f $(SHIM_LIB) $(DEMO_BIN)
	@echo "Cleaned build artifacts"

# Install (to /usr/local by default)
PREFIX ?= /usr/local
install: $(SHIM_LIB)
	install -d $(PREFIX)/lib
	install -m 644 $(SHIM_LIB) $(PREFIX)/lib/
	install -d $(PREFIX)/include
	install -m 644 tiered_memory.h $(PREFIX)/include/
	@echo "Installed to $(PREFIX)"

# Uninstall
uninstall:
	rm -f $(PREFIX)/lib/$(SHIM_LIB)
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
	@echo "  ./tiered_manager     # Run demo"
	@echo "  LD_PRELOAD=./libmmap_shim.so ./app  # Use shim"

.PHONY: all lib demo debug clean install uninstall help
