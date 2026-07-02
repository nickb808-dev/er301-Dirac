# Makefile — MorphGrain ER-301 package
#
# Cross-compiles Dirac.cpp + SWIG wrapper into
# lib/am335x/libdirac.so for the ER-301 (AM3358, ARM Cortex-A8).
#
# Follows er-301/scripts/tutorial.mk exactly for am335x flags:
#   - ARM flags: -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -mabi=aapcs
#                -Dfar= -D__DYNAMIC_REENT__
#   - Linker: -nostdlib -nodefaultlibs -r  (relocatable ET_REL, NOT -shared)
#   - No -fPIC for am335x!  The firmware's ELF loader uses absolute relocations;
#     -fPIC generates GOT-relative code + _GLOBAL_OFFSET_TABLE_ which the loader
#     cannot resolve.
#   - SWIG flags: -no-old-metatable-bindings -nomoduleglobal -small -fvirtual -fcompact
#   - Lua headers: libs/lua54  (the ER-301 firmware runs Lua 5.4)
#
# ── PREREQUISITES ────────────────────────────────────────────────────────────
#
#   macOS (recommended path):
#     Install Docker Desktop, then:
#       make docker-image
#       make swig-docker ER301_SDK=~/er-301
#       make docker-build ER301_SDK=~/er-301
#
#   Linux / CI (native cross-compiler):
#     sudo apt-get install swig gcc-arm-none-eabi binutils-arm-none-eabi \
#                          libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
#     make TOOLCHAIN=native ER301_SDK=/path/to/er-301
#
# ── VARIABLES ─────────────────────────────────────────────────────────────────

# Path to the cloned odevices/er-301 repo (provides SDK headers).
ER301_SDK ?= $(HOME)/er-301

# Set TOOLCHAIN=native to use arm-none-eabi-g++ directly (Linux / CI).
# Default uses Docker (macOS-friendly).
TOOLCHAIN ?= docker

PKG     := dirac
MODULE  := libdirac
VERSION := 0.1.18
ARCH    := am335x

SRCDIR  := src
OUTDIR  := lib/$(ARCH)
OUTLIB  := $(OUTDIR)/$(MODULE).so
OBJS_DIR := $(OUTDIR)/obj

SWIG_FILE := $(SRCDIR)/$(MODULE).swig
SWIG_WRAP := $(SRCDIR)/$(MODULE)_wrap.cpp

# ── COMPILER FLAGS ────────────────────────────────────────────────────────────

# Common flags shared by all compilation units.
CXXFLAGS_COMMON := \
	-std=c++11 \
	-ffunction-sections \
	-fdata-sections \
	-ffast-math \
	-fno-builtin-sincosf \
	-fno-stack-protector \
	-fno-exceptions \
	-D__DYNAMIC_REENT__ \
	-mabi=aapcs \
	-DNDEBUG \
	-D_GLIBCXX_USE_CXX11_ABI=0 \
	-I$(ER301_SDK) \
	-I$(ER301_SDK)/libs/lua54 \
	-I$(SRCDIR) \
	-Wall \
	-Wno-unused-parameter

# ARM Cortex-A8 flags — from tutorial.mk's CFLAGS.am335x.
CXXFLAGS_ARM := \
	-mcpu=cortex-a8 \
	-mfpu=neon \
	-mfloat-abi=hard \
	-Dfar=

# DSP code: optimise for speed + NEON vectorisation.
CXXFLAGS_DSP := $(CXXFLAGS_COMMON) -O2 $(CXXFLAGS_ARM)

# SWIG wrapper: optimise for size (lots of generated boilerplate).
# NOTE: NO -include compat_swig.h here — the #define fprintf macro conflicts
# with `using ::fprintf` in <cstdio>.  fprintf references in the wrap .o are
# resolved by compat.cpp at partial-link time.
CXXFLAGS_WRAP := $(CXXFLAGS_COMMON) -Os $(CXXFLAGS_ARM)

# compat.cpp: compiled WITHOUT -ffast-math so GCC does not combine
# sinf+cosf back into a recursive sincosf call inside our sincosf stub.
CXXFLAGS_COMPAT := \
	-std=c++11 \
	-ffunction-sections \
	-fdata-sections \
	-fno-stack-protector \
	-fno-exceptions \
	-D__DYNAMIC_REENT__ \
	-mabi=aapcs \
	-DNDEBUG \
	-D_GLIBCXX_USE_CXX11_ABI=0 \
	-I$(ER301_SDK) \
	-I$(SRCDIR) \
	-O1 \
	$(CXXFLAGS_ARM)

# Partial-link (relocatable ET_REL).  The ER-301 firmware's custom ELF loader
# resolves all symbols from its own static export table at load time.
LDFLAGS := -nostdlib -nodefaultlibs -r

# ── TOOLCHAIN ─────────────────────────────────────────────────────────────────

CXX   := arm-none-eabi-g++
STRIP := arm-none-eabi-strip

DOCKER_IMAGE := er301-crosscompile:latest

# ── OBJECT FILES ──────────────────────────────────────────────────────────────

OBJ_MGS     := $(OBJS_DIR)/Dirac.o
OBJ_MGHD    := $(OBJS_DIR)/DiracHeadDisplay.o
OBJ_WRAP   := $(OBJS_DIR)/libdirac_wrap.o
OBJ_COMPAT := $(OBJS_DIR)/compat.o

# ── PHONY TARGETS ─────────────────────────────────────────────────────────────

.PHONY: all swig build docker-image docker-build swig-docker pkg dist clean help

all: help

# ── dist: one-shot cross-compile + package ────────────────────────────────────
# `make pkg` alone fails on macOS because the .so prerequisite falls back to a
# native arm-none-eabi compile (no host toolchain).  `dist` runs the Docker
# cross-build first, then packages — the reliable one-command path.
# REMINDER: bump VERSION (Makefile + assets/toc.lua) on every change you flash —
# the ER-301 caches the .so per version, so a same-version reinstall can keep
# running the stale binary.
dist:
	$(MAKE) swig-docker  ER301_SDK=$(ER301_SDK)
	$(MAKE) docker-build ER301_SDK=$(ER301_SDK)
	$(MAKE) pkg

# ── SWIG: generate the Lua binding wrapper ────────────────────────────────────
# Use `make swig-docker` instead if you don't have swig on your Mac.

swig: $(SWIG_WRAP)

$(SWIG_WRAP): $(SWIG_FILE) $(SRCDIR)/Dirac.h $(SRCDIR)/DiracHeadDisplay.h $(SRCDIR)/DiracFieldGraphic.h
	@echo ">>> SWIG: generating Lua wrapper..."
	swig -c++ -lua \
		-no-old-metatable-bindings \
		-nomoduleglobal \
		-small \
		-fvirtual \
		-fcompact \
		-I$(ER301_SDK) \
		-I$(SRCDIR) \
		-o $@ $<
	@echo ">>> SWIG done: $@"

# ── TWO-PHASE BUILD ───────────────────────────────────────────────────────────
# Phase 1: compile each .cpp → .o
# Phase 2: partial-link all .o → .so

build: $(OUTLIB)
	@echo ">>> Built: $(OUTLIB)"

$(OUTDIR):
	mkdir -p $@

$(OBJS_DIR): | $(OUTDIR)
	mkdir -p $@

$(OBJ_MGS): $(SRCDIR)/Dirac.cpp $(SRCDIR)/Dirac.h | $(OBJS_DIR)
	@echo ">>> CC Dirac.cpp"
	$(CXX) $(CXXFLAGS_DSP) -c -o $@ $<

$(OBJ_MGHD): $(SRCDIR)/DiracHeadDisplay.cpp $(SRCDIR)/DiracHeadDisplay.h $(SRCDIR)/Dirac.h | $(OBJS_DIR)
	@echo ">>> CC DiracHeadDisplay.cpp"
	$(CXX) $(CXXFLAGS_DSP) -c -o $@ $<

$(OBJ_WRAP): $(SWIG_WRAP) | $(OBJS_DIR)
	@echo ">>> CC libdirac_wrap.cpp"
	$(CXX) $(CXXFLAGS_WRAP) -c -o $@ $<

$(OBJ_COMPAT): $(SRCDIR)/compat.cpp | $(OBJS_DIR)
	@echo ">>> CC compat.cpp"
	$(CXX) $(CXXFLAGS_COMPAT) -c -o $@ $<

$(OUTLIB): $(OBJ_MGS) $(OBJ_MGHD) $(OBJ_WRAP) $(OBJ_COMPAT) | $(OUTDIR)
	@echo ">>> LINK (relocatable) $(OUTLIB)"
	$(CXX) $(LDFLAGS) -o $@ $(OBJ_MGS) $(OBJ_MGHD) $(OBJ_WRAP) $(OBJ_COMPAT)
	$(STRIP) --strip-unneeded $(OUTLIB)

# ── DOCKER: build the cross-compile image ────────────────────────────────────

docker-image:
	docker build -t $(DOCKER_IMAGE) -f Dockerfile .

# ── SWIG inside Docker ────────────────────────────────────────────────────────
# Use this on macOS instead of `make swig` (no local swig needed).

swig-docker: docker-image
	$(eval SDK_ABS := $(shell realpath $(ER301_SDK) 2>/dev/null))
	@test -n "$(SDK_ABS)" || \
		{ echo "ERROR: ER301_SDK path '$(ER301_SDK)' does not exist."; \
		  echo "       Clone it first:  git clone https://github.com/odevices/er-301 ~/er-301"; \
		  exit 1; }
	@echo ">>> SWIG (inside Docker) ..."
	docker run --rm \
		-v "$(CURDIR)":/build \
		-v "$(SDK_ABS)":/er301_sdk \
		-w /build \
		$(DOCKER_IMAGE) \
		swig -c++ -lua \
			-no-old-metatable-bindings \
			-nomoduleglobal \
			-small \
			-fvirtual \
			-fcompact \
			-I/er301_sdk \
			-Isrc \
			-o $(SWIG_WRAP) $(SWIG_FILE)
	@echo ">>> SWIG done: $(SWIG_WRAP)"

# ── DOCKER BUILD ──────────────────────────────────────────────────────────────
# Cross-compiles inside Docker; mounts this dir and the SDK live.

docker-build: docker-image | $(OUTDIR)
	@test -f "$(SWIG_WRAP)" || \
		{ echo "ERROR: SWIG wrapper not generated yet."; \
		  echo "       Run first:  make swig-docker ER301_SDK=~/er-301"; \
		  exit 1; }
	$(eval SDK_ABS := $(shell realpath $(ER301_SDK) 2>/dev/null))
	@test -n "$(SDK_ABS)" || \
		{ echo "ERROR: ER301_SDK path '$(ER301_SDK)' does not exist."; \
		  echo "       Clone it first:  git clone https://github.com/odevices/er-301 ~/er-301"; \
		  echo "       Then retry:      make docker-build ER301_SDK=~/er-301"; \
		  exit 1; }
	@echo ">>> Docker cross-compile: $(OUTLIB) (SDK=$(SDK_ABS)) ..."
	docker run --rm \
		-v "$(CURDIR)":/build \
		-v "$(SDK_ABS)":/er301_sdk \
		-w /build \
		$(DOCKER_IMAGE) \
		make build TOOLCHAIN=native ER301_SDK=/er301_sdk
	@echo ">>> Done: $(OUTLIB)"

# ── PACKAGE ───────────────────────────────────────────────────────────────────
# Assembles the installable .pkg file (a flat zip containing the Lua assets
# and the compiled .so).  Run after docker-build.

PKGDIR  := build/$(ARCH)
PKGFILE := $(PKGDIR)/$(PKG)-$(VERSION).pkg

pkg: $(PKGFILE)

$(PKGFILE): $(OUTLIB) assets/toc.lua assets/Dirac.lua assets/WaveHeadView.lua assets/GrainFieldView.lua | $(PKGDIR)
	@echo ">>> PKG $(PKGFILE)"
	cd assets && zip -j ../$(PKGFILE) toc.lua Dirac.lua WaveHeadView.lua GrainFieldView.lua
	cd $(OUTDIR) && zip -j ../../$(PKGFILE) libdirac.so
	@echo ">>> Done: $(PKGFILE)"

$(PKGDIR):
	mkdir -p $@

# ── CLEAN ─────────────────────────────────────────────────────────────────────

clean:
	rm -f $(SWIG_WRAP) $(OUTLIB)
	rm -rf $(OBJS_DIR)

# ── HELP ──────────────────────────────────────────────────────────────────────

help:
	@echo ""
	@echo "MorphGrain build targets (recommended order for macOS):"
	@echo "  make docker-image                           Build the Docker image (once)"
	@echo "  make swig-docker ER301_SDK=~/er-301         Generate SWIG wrapper (inside Docker)"
	@echo "  make docker-build ER301_SDK=~/er-301        Cross-compile inside Docker"
	@echo "  make pkg                                    Package into build/am335x/dirac-$(VERSION).pkg"
	@echo ""
	@echo "Alternatives:"
	@echo "  make swig ER301_SDK=~/er-301                Generate SWIG wrapper (host, needs brew install swig)"
	@echo "  make build TOOLCHAIN=native ER301_SDK=...   Cross-compile natively (Linux only)"
	@echo "  make clean                                  Remove generated files"
	@echo ""
