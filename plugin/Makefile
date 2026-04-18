# Makefile for QEMU TCG tracing plugin and trace inspector
#
# Dependencies:
#   - QEMU install or source tree (for qemu-plugin.h header)
#   - libzstd-dev (for zstd headers and trace_inspector linking)
#   - glib-2.0 (for plugin build)
#
# Usage:
#   make                                     # build both plugin and inspector
#   make QEMU_PREFIX=~/qemu-custom           # QEMU install prefix (default)
#   make QEMU_PREFIX=~/qemu-9.2.4/build      # or a QEMU build directory
#   make plugin                              # build only the TCG plugin
#   make inspector                           # build only the trace inspector
#   make clean

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter

# Where to find qemu-plugin.h. Works with both:
#   - QEMU install prefix (header at $QEMU_PREFIX/include/qemu-plugin.h)
#   - QEMU source tree    (header at $QEMU_PREFIX/include/qemu/qemu-plugin.h)
QEMU_PREFIX ?= $(HOME)/qemu-custom

# Targets
PLUGIN    = champsim_tracer.so
INSPECTOR = trace_inspector

# pkg-config flags
GLIB_CFLAGS  := $(shell pkg-config --cflags glib-2.0 2>/dev/null)
GLIB_LDFLAGS := $(shell pkg-config --libs glib-2.0 2>/dev/null)
ZSTD_CFLAGS  := $(shell pkg-config --cflags libzstd 2>/dev/null)
ZSTD_LDFLAGS := $(shell pkg-config --libs libzstd 2>/dev/null || echo "-lzstd")

# Auto-detect qemu-plugin.h location
QEMU_PLUGIN_INC := $(shell \
	if [ -f "$(QEMU_PREFIX)/include/qemu-plugin.h" ]; then \
		echo "-I$(QEMU_PREFIX)/include"; \
	elif [ -f "$(QEMU_PREFIX)/include/qemu/qemu-plugin.h" ]; then \
		echo "-I$(QEMU_PREFIX)/include/qemu"; \
	fi)

# ─────────────────────────────────────────────

.PHONY: all plugin inspector clean check-deps

all: check-deps plugin inspector

plugin: check-deps $(PLUGIN)

inspector: check-deps $(INSPECTOR)

$(PLUGIN): champsim_tracer.c
	@if [ -z "$(QEMU_PLUGIN_INC)" ]; then \
		echo "ERROR: Cannot find qemu-plugin.h under $(QEMU_PREFIX)"; \
		echo "Searched:"; \
		echo "  $(QEMU_PREFIX)/include/qemu-plugin.h"; \
		echo "  $(QEMU_PREFIX)/include/qemu/qemu-plugin.h"; \
		echo ""; \
		echo "Set QEMU_PREFIX to your QEMU install or source tree:"; \
		echo "  make QEMU_PREFIX=~/qemu-custom"; \
		exit 1; \
	fi
	$(CC) $(CFLAGS) -shared -fPIC \
		$(QEMU_PLUGIN_INC) \
		$(GLIB_CFLAGS) $(ZSTD_CFLAGS) \
		-o $@ $< \
		$(GLIB_LDFLAGS)
	@echo "Built: $@"

$(INSPECTOR): trace_inspector.c
	$(CC) $(CFLAGS) $(ZSTD_CFLAGS) -o $@ $< $(ZSTD_LDFLAGS)
	@echo "Built: $@"

check-deps:
	@pkg-config --exists glib-2.0 2>/dev/null || \
		(echo "ERROR: glib-2.0 not found. Install: sudo apt install libglib2.0-dev" && exit 1)
	@pkg-config --exists libzstd 2>/dev/null || \
		(echo "ERROR: libzstd not found. Install: sudo apt install libzstd-dev" && exit 1)

clean:
	rm -f $(PLUGIN) $(INSPECTOR)
