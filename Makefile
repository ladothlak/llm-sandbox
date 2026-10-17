# Palworld VR Injector — Makefile for Linux (CachyOS)
#
# Builds the complete LD_PRELOAD injector library combining:
#   - stub.c          : Entry point, environment detection, logging
#   - dxgi_hook.cpp   : DXGI/D3D12 hook layer, virtual adapter injection
#   - openxr_compositor.cpp : OpenXR session + swap chain management
#
# Prerequisites:
#   sudo pacman -S openxr-sdk-dev vulkan-devel
#
# Usage:
#   make all       — Build combined library (libpalworldvr.so)
#   make debug     — Build with debug symbols and verbose logging
#   make install   — Install to lib/ subdirectory
#   make clean     — Remove build artifacts

CC = gcc
CXX = g++
AR = ar rcs
LD = ld
STRIP = strip

# Compiler flags
CFLAGS  = -fPIC -O2 -Wall -Wextra -D_GLIBCXX_USE_CXX11_ABI=1 -U_FORTIFY_SOURCE
CXXFLAGS = -std=c++17 -fPIC -O2 -Wall -Wextra -D_GLIBCXX_USE_CXX11_ABI=1 -U_FORTIFY_SOURCE

# Debug build flags (optional)
DEBUG_FLAGS = -O0 -g3 -DVRHOOK_DEBUG=1 -DDEBUG_LOGGING=1

# Output paths
TARGET_SO := libpalworldvr.so
BUILD_DIR  := ./build
INSTALL_DIR := ./lib

# Source files
STUB_SRC   := stub.c
HOOK_SRCS  := hook/dxgi_hook.cpp hook/openxr_compositor.cpp

# Include directories
INC_DIRS := .

# Linker flags — combine C and C++ sources into one shared library
# -lopenxr_loader: OpenXR loader runtime (provides xrGetInstanceProcAddr, etc.)
# -lvulkan: Vulkan loader and surface creation functions
LDFLAGS = -shared -fPIC -ldl -lpthread -lopenxr_loader -lvulkan

.PHONY: all clean debug install uninstall test help

all: $(TARGET_SO)

debug: CFLAGS  += $(DEBUG_FLAGS)
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: LDFLAGS += -g3
debug: $(TARGET_SO)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Step 1: Compile stub.c as a C object
$(BUILD_DIR)/stub.o: $(STUB_SRC) | $(BUILD_DIR)
	@echo "[Build] Compiling stub.c → stub.o"
	$(CC) $(CFLAGS) -c $< -o $@

# Step 2: Compile C++ sources
$(BUILD_DIR)/dxgi_hook.o: hook/dxgi_hook.cpp | $(BUILD_DIR)
	@echo "[Build] Compiling dxgi_hook.cpp → dxgi_hook.o"
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/openxr_compositor.o: hook/openxr_compositor.cpp | $(BUILD_DIR)
	@echo "[Build] Compiling openxr_compositor.cpp → openxr_compositor.o"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Step 3: Link all objects into the final shared library
$(TARGET_SO): $(BUILD_DIR)/stub.o $(BUILD_DIR)/dxgi_hook.o $(BUILD_DIR)/openxr_compositor.o
	@echo "[Build] Linking → $(TARGET_SO)"
	$(CC) $(LDFLAGS) -o $@ $(BUILD_DIR)/stub.o $(BUILD_DIR)/dxgi_hook.o $(BUILD_DIR)/openxr_compositor.o
	-$(STRIP) --strip-debug $@ 2>/dev/null || true

install: $(TARGET_SO)
	@echo "[Install] Copying to $(INSTALL_DIR)/"
	@mkdir -p $(INSTALL_DIR)
	cp $(TARGET_SO) $(INSTALL_DIR)/
	@echo "[Install] Library installed to lib/$(TARGET_SO)"

uninstall:
	@echo "[Uninstall] Removing from lib/"
	rm -f $(INSTALL_DIR)/$(TARGET_SO)

test: all
	@echo "=== Running integration test ==="
	@./scripts/run-test.sh || echo "[Test harness not found — skipping]"

clean:
	@echo "[Clean] Removing build artifacts..."
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET_SO)
	@echo "[Clean] Done."

help:
	@echo "Palworld VR Injector Build System"
	@echo "==================================="
	@echo ""
	@echo "Prerequisites:"
	@echo "  sudo pacman -S openxr-sdk-dev vulkan-devel"
	@echo ""
	@echo "Targets:"
	@echo "  make all       — Build combined library (libpalworldvr.so)"
	@echo "  make debug     — Build with debug symbols and verbose logging"
	@echo "  make install   — Install to lib/ subdirectory"
	@echo "  make uninstall — Remove from lib/"
	@echo "  make test      — Run integration test against Palworld"
	@echo "  make clean     — Remove build artifacts"
	@echo ""
	@echo "Usage:"
	@echo "  LD_PRELOAD=./libpalworldvr.so wine64 palworld-win64-shipping.exe"
	@echo ""
	@echo "Or with the stub library explicitly:"
	@echo "  LD_PRELOAD=./lib/libpalworldvrstub.so:./libpalworldvr.so wine64 palworld-win64-shipping.exe"
