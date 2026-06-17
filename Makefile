# TARGET: native (default) | moddwarf-new | rpi5
TARGET ?= native

# ── Synthesis mode flags ───────────────────────────────────────────────────
# MEGALO_HN_SYNTH=1  → harmonic+noise additive synthesis (default on this branch)
# MEGALO_RAVE=1      → RAVE neural decoder stub (rpi5 only; needs ONNX RT)
MEGALO_HN_SYNTH ?= 1
MEGALO_RAVE     ?= 0

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
    # mod-plugin-builder injects CXX and CXXFLAGS via command-line args.
    # Use ?= so those take precedence; fallbacks serve only for manual builds.
    CXX      ?= aarch64-modaudio-linux-gnu-g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math \
                -mcpu=cortex-a35 \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
    EXTRA_DEFS = -DMEGALO_PV_N=1024

else  # native
    CXX      ?= g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
    EXTRA_DEFS =
endif

ifeq ($(MEGALO_HN_SYNTH),1)
    EXTRA_DEFS += -DMEGALO_HN_SYNTH
    ifeq ($(MEGALO_RAVE),1)
        EXTRA_DEFS += -DMEGALO_RAVE
        # LDFLAGS    += -lonnxruntime   # uncomment once ONNX RT is installed
    endif
endif

# ── Polyphonic analysis quality tier ───────────────────────────────────────
# MEGALO_HN_QUALITY = dwarf | pi5 | desktop  (FFT size, max notes/partials).
# Defaults follow the target: Dwarf→dwarf, rpi5→pi5, native→desktop.
ifeq ($(TARGET),moddwarf-new)
    MEGALO_HN_QUALITY ?= dwarf
else ifeq ($(TARGET),rpi5)
    MEGALO_HN_QUALITY ?= pi5
else
    MEGALO_HN_QUALITY ?= desktop
endif
ifeq ($(MEGALO_HN_QUALITY),dwarf)
    EXTRA_DEFS += -DMEGALO_HN_QUALITY=0
else ifeq ($(MEGALO_HN_QUALITY),pi5)
    EXTRA_DEFS += -DMEGALO_HN_QUALITY=1
else
    EXTRA_DEFS += -DMEGALO_HN_QUALITY=2
endif

# In cross-compilation CXXFLAGS already contains -I$(STAGING_DIR)/usr/include
LV2FLAGS ?= $(shell pkg-config --cflags lv2 2>/dev/null)

# Cohabitation build: distinct bundle/binary (MegaloHN) so it installs
# alongside the stock Megalo. Same host-agnostic core (megalo_dsp.cpp) as
# master; the polyphonic H+N engine lives in the core, gated by MEGALO_HN_SYNTH.
BUNDLE  = megaloHN.lv2
BINARY  = $(BUNDLE)/megaloHN.so
SOURCES = src/plugin.cpp src/megalo_dsp.cpp src/glibc_compat.cpp
HEADERS = src/megalo_dsp.h src/freeze_engine.hpp src/granular_looper.hpp \
          src/biquad.hpp src/envelope.hpp src/phase_vocoder.hpp \
          src/hn_state.hpp src/hn_quality.hpp src/hn_fft.hpp src/hn_multif0.hpp \
          src/hn_nnls.hpp src/additive_synth.hpp src/hn_poly_synth.hpp src/rave_engine.hpp

all: $(BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(EXTRA_DEFS) $(LV2FLAGS) -fPIC -shared -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(BINARY)

# Recursive copy installs everything inside the bundle: the .so binary,
# the manifest + plugin TTLs, the preset TTLs (Choir / Fast / Grainy …),
# and the entire modgui/ directory (icon HTML, stylesheet, script, and
# screenshot/thumbnail PNGs when present).
install: $(BINARY)
	install -d $(DESTDIR)/usr/lib/lv2
	cp -r $(BUNDLE) $(DESTDIR)/usr/lib/lv2/

.PHONY: all clean install
