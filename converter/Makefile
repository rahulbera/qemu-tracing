# Makefile for raw2champsim — raw QEMU trace → ChampSim v2 converter
#
# Dependencies:
#   - libzstd (system): install via `apt install libzstd-dev` or equivalent
#   - Zydis (fetched automatically): x86 instruction decoder
#
# Usage:
#   make            # build the converter (fetches Zydis if needed)
#   make clean      # remove build artifacts (keeps Zydis)
#   make distclean  # remove everything including Zydis

CC       ?= gcc
CFLAGS   ?= -O2 -Wall
TARGET    = raw2champsim

# Zydis configuration
ZYDIS_VERSION = v4.1.1
ZYDIS_REPO    = https://github.com/zyantific/zydis.git
ZYDIS_DIR     = zydis
ZYDIS_BUILD   = $(ZYDIS_DIR)/build
ZYDIS_LIB     = $(ZYDIS_BUILD)/libZydis.a
ZYCORE_LIB    = $(ZYDIS_BUILD)/zycore/libZycore.a
ZYDIS_INC     = -I$(ZYDIS_DIR)/include \
                -I$(ZYDIS_DIR)/dependencies/zycore/include \
                -I$(ZYDIS_BUILD) \
                -I$(ZYDIS_BUILD)/zycore

# zstd (must be installed on the system)
ZSTD_CFLAGS  := $(shell pkg-config --cflags libzstd 2>/dev/null)
ZSTD_LDFLAGS := $(shell pkg-config --libs libzstd 2>/dev/null || echo "-lzstd")

# ─────────────────────────────────────────────

.PHONY: all clean distclean check-zstd

all: check-zstd $(TARGET)

$(TARGET): raw2champsim.c $(ZYDIS_LIB) $(ZYCORE_LIB)
	$(CC) $(CFLAGS) $(ZYDIS_INC) $(ZSTD_CFLAGS) -o $@ $< \
		$(ZYDIS_LIB) $(ZYCORE_LIB) $(ZSTD_LDFLAGS)
	@echo ""
	@echo "Built: $(TARGET)"

# ── Zydis: clone and build if not present ────

$(ZYDIS_LIB) $(ZYCORE_LIB): | $(ZYDIS_BUILD)
	@echo "Building Zydis..."
	cd $(ZYDIS_BUILD) && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DZYDIS_BUILD_TOOLS=OFF \
		-DZYDIS_BUILD_EXAMPLES=OFF \
		-DZYDIS_BUILD_DOXYGEN=OFF
	$(MAKE) -C $(ZYDIS_BUILD) -j$$(nproc)
	@echo "Zydis built successfully."

$(ZYDIS_BUILD): | $(ZYDIS_DIR)
	mkdir -p $(ZYDIS_BUILD)

$(ZYDIS_DIR):
	@echo "Fetching Zydis $(ZYDIS_VERSION)..."
	git clone --recursive --branch $(ZYDIS_VERSION) --depth 1 \
		$(ZYDIS_REPO) $(ZYDIS_DIR)

# ── System dependency check ──────────────────

check-zstd:
	@pkg-config --exists libzstd 2>/dev/null || \
		(echo "ERROR: libzstd-dev not found. Install it:" && \
		 echo "  Ubuntu/Debian: sudo apt install libzstd-dev" && \
		 echo "  Fedora/RHEL:   sudo dnf install libzstd-devel" && \
		 echo "  macOS:         brew install zstd" && \
		 exit 1)

# ── Cleanup ──────────────────────────────────

clean:
	rm -f $(TARGET)

distclean: clean
	rm -rf $(ZYDIS_DIR)
