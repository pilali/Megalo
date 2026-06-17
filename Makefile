# TARGET: native (default) | moddwarf-new | rpi5
TARGET ?= native

# ── Synthesis mode flags ───────────────────────────────────────────────────
# MEGALO_HN_SYNTH=1  → harmonic+noise additive synthesis (both targets)
# MEGALO_RAVE=1      → RAVE neural decoder stub (rpi5 only; needs ONNX RT)
MEGALO_HN_SYNTH ?= 0
MEGALO_RAVE     ?= 0

# ── Per-target defaults ────────────────────────────────────────────────────
ifeq ($(TARGET),rpi5)
    CXX      ?= aarch64-linux-gnu-g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math -funroll-loops \
                -mcpu=cortex-a76 -march=armv8.2-a \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
    EXTRA_DEFS = -DMEGALO_PHASE_VOCODER -DMEGALO_PV_N=2048

else ifeq ($(TARGET),moddwarf-new)
    # CXX and CXXFLAGS are injected by mod-plugin-builder (megalo.mk).
    # This target can also be used standalone with the cross-compiler on PATH.
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

BUNDLE  = megaloHN.lv2
BINARY  = $(BUNDLE)/megaloHN.so
SOURCES = src/plugin.cpp
HEADERS = src/freeze_engine.hpp src/granular_looper.hpp src/biquad.hpp src/envelope.hpp \
          src/hn_quality.hpp src/hn_fft.hpp src/hn_multif0.hpp \
          src/phase_vocoder.hpp src/hn_analyzer.hpp src/additive_synth.hpp src/rave_engine.hpp

all: $(BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(EXTRA_DEFS) $(LV2FLAGS) -fPIC -shared -o $@ $(SOURCES)

clean:
	rm -f $(BINARY)

install: $(BINARY)
	install -d $(DESTDIR)/usr/lib/lv2/$(BUNDLE)
	cp $(BUNDLE)/*.so $(BUNDLE)/*.ttl $(DESTDIR)/usr/lib/lv2/$(BUNDLE)/

.PHONY: all clean install
