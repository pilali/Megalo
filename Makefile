# TARGET: native (default) | moddwarf-new | rpi5
TARGET ?= native

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

# In cross-compilation CXXFLAGS already contains -I$(STAGING_DIR)/usr/include
LV2FLAGS ?= $(shell pkg-config --cflags lv2 2>/dev/null)

BUNDLE  = megalo.lv2
BINARY  = $(BUNDLE)/megalo.so
SOURCES = src/plugin.cpp
HEADERS = src/freeze_engine.hpp src/biquad.hpp src/envelope.hpp src/phase_vocoder.hpp

all: $(BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(EXTRA_DEFS) $(LV2FLAGS) -fPIC -shared -o $@ $(SOURCES)

clean:
	rm -f $(BINARY)

install: $(BINARY)
	install -d $(DESTDIR)/usr/lib/lv2/$(BUNDLE)
	cp $(BUNDLE)/*.so $(BUNDLE)/*.ttl $(DESTDIR)/usr/lib/lv2/$(BUNDLE)/

.PHONY: all clean install
