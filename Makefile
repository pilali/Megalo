# TARGET: native (default) | moddwarf-new | rpi5
TARGET ?= native

# ── Synthesis mode flags ───────────────────────────────────────────────────
# This Makefile builds TWO plugins from the one shared core: the stock granular
# `megalo` (HN off) and the polyphonic `megaloHN` (HN on). The MEGALO_HN_SYNTH
# flag is therefore applied per-target (see HN_DEFS below), not globally.
# MEGALO_RAVE=1 → RAVE neural decoder stub for megaloHN (rpi5 only; needs ONNX RT)
MEGALO_RAVE ?= 0

# ── Per-target defaults ────────────────────────────────────────────────────
# override is needed so cross-compilation targets win over the environment CXX.
ifeq ($(TARGET),rpi5)
    override CXX      := aarch64-linux-gnu-g++
    # -fno-tree-vectorize avoids emitting calls to libmvec (vectorized math),
    # which isn't packaged on the RPi5 mod/pistomp Buildroot system.
    override CXXFLAGS := -std=c++17 -O3 -ffast-math -funroll-loops \
                         -fno-tree-vectorize \
                         -mcpu=cortex-a76 -march=armv8.2-a \
                         -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
    # Static libstdc++/libgcc: embed the C++ runtime so the .so doesn't depend
    # on the target system's libstdc++ version (Ubuntu's aarch64-linux-gnu-g++
    # uses GLIBCXX symbols that mod-system's Buildroot libstdc++ doesn't have).
    override LDFLAGS  := -static-libstdc++ -static-libgcc
    EXTRA_DEFS = -DMEGALO_PHASE_VOCODER -DMEGALO_PV_N=2048

else ifeq ($(TARGET),moddwarf-new)
    # MOD Dwarf — Cortex-A35, the most constrained target. Granular-only pitch
    # path (no phase vocoder) and the conservative polyphony tier.
    # mod-plugin-builder injects CXX and CXXFLAGS via command-line args.
    # Use ?= so those take precedence; fallbacks serve only for manual builds.
    CXX      ?= aarch64-modaudio-linux-gnu-g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math \
                -mcpu=cortex-a35 \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
    EXTRA_DEFS = -DMEGALO_PV_N=1024

else ifeq ($(TARGET),modduox-new)
    # MOD Duo X — quad Cortex-A53 (ARMv8-A). No Dwarf restrictions: full
    # features like the Pi 5 / desktop builds (phase vocoder + pi5 polyphony
    # tier; see the quality-tier block below).
    CXX      ?= aarch64-modaudio-linux-gnu-g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math \
                -mcpu=cortex-a53 \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
    EXTRA_DEFS = -DMEGALO_PHASE_VOCODER -DMEGALO_PV_N=2048

else  # native
    CXX      ?= g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
    EXTRA_DEFS =
endif

# ── Harmonic+Noise engine defines (megaloHN only) ──────────────────────────
# These flags are applied ONLY to the MegaloHN binary; the stock Megalo binary
# is compiled from the same sources without them, so its #ifdef MEGALO_HN_SYNTH
# blocks compile out and it keeps the granular freeze behaviour.
HN_DEFS = -DMEGALO_HN_SYNTH
ifeq ($(MEGALO_RAVE),1)
    HN_DEFS += -DMEGALO_RAVE
    # LDFLAGS += -lonnxruntime   # uncomment once ONNX RT is installed
endif

# ── Polyphonic analysis quality tier ───────────────────────────────────────
# MEGALO_HN_QUALITY = dwarf | pi5 | desktop  (FFT size, max notes/partials).
# Defaults follow the target: Dwarf→dwarf, Duo X / rpi5→pi5, native→desktop.
# Tier only affects the HN engine, so it lives in HN_DEFS.
ifeq ($(TARGET),moddwarf-new)
    MEGALO_HN_QUALITY ?= dwarf
else ifeq ($(TARGET),modduox-new)
    MEGALO_HN_QUALITY ?= pi5
else ifeq ($(TARGET),rpi5)
    MEGALO_HN_QUALITY ?= pi5
else
    MEGALO_HN_QUALITY ?= desktop
endif
ifeq ($(MEGALO_HN_QUALITY),dwarf)
    HN_DEFS += -DMEGALO_HN_QUALITY=0
else ifeq ($(MEGALO_HN_QUALITY),pi5)
    HN_DEFS += -DMEGALO_HN_QUALITY=1
else
    HN_DEFS += -DMEGALO_HN_QUALITY=2
endif

# In cross-compilation CXXFLAGS already contains -I$(STAGING_DIR)/usr/include
LV2FLAGS ?= $(shell pkg-config --cflags lv2 2>/dev/null)

# Two distinct plugins built from one shared, host-agnostic core
# (src/megalo_dsp.cpp). The polyphonic H+N engine lives in the core gated by
# MEGALO_HN_SYNTH; flipping that flag (via HN_DEFS) yields MegaloHN, leaving it
# off yields the stock granular Megalo. Both ship as separate LV2 bundles so
# they install side by side.
STD_BUNDLE  = megalo.lv2
STD_BINARY  = $(STD_BUNDLE)/megalo.so
HN_BUNDLE   = megaloHN.lv2
HN_BINARY   = $(HN_BUNDLE)/megaloHN.so

SOURCES = src/plugin.cpp src/megalo_dsp.cpp src/glibc_compat.cpp
HEADERS = src/megalo_dsp.h src/freeze_engine.hpp src/granular_looper.hpp \
          src/biquad.hpp src/envelope.hpp src/phase_vocoder.hpp \
          src/hn_state.hpp src/hn_quality.hpp src/hn_fft.hpp src/hn_multif0.hpp \
          src/hn_nnls.hpp src/additive_synth.hpp src/hn_poly_synth.hpp src/rave_engine.hpp

# Build both plugins by default. Use `make megalo` / `make megaloHN` to build
# just one (e.g. mod-plugin-builder per-package builds).
all: $(STD_BINARY) $(HN_BINARY)

megalo:   $(STD_BINARY)
megaloHN: $(HN_BINARY)

$(STD_BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(EXTRA_DEFS) $(LV2FLAGS) -fPIC -shared -o $@ $(SOURCES) $(LDFLAGS)

$(HN_BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(EXTRA_DEFS) $(HN_DEFS) $(LV2FLAGS) -fPIC -shared -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(STD_BINARY) $(HN_BINARY)

# Recursive copy installs everything inside each bundle: the .so binary,
# the manifest + plugin TTLs, the preset TTLs (Choir / Fast / Grainy …),
# and the entire modgui/ directory (icon HTML, stylesheet, script, and
# screenshot/thumbnail PNGs when present).
install: install-megalo install-megaloHN

install-megalo: $(STD_BINARY)
	install -d $(DESTDIR)/usr/lib/lv2
	cp -r $(STD_BUNDLE) $(DESTDIR)/usr/lib/lv2/

install-megaloHN: $(HN_BINARY)
	install -d $(DESTDIR)/usr/lib/lv2
	cp -r $(HN_BUNDLE) $(DESTDIR)/usr/lib/lv2/

.PHONY: all megalo megaloHN clean install install-megalo install-megaloHN
