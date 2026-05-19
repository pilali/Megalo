# TARGET: native (default) | moddwarf-new | rpi5
TARGET ?= native

# ── Per-target defaults ────────────────────────────────────────────────────
# override is needed so cross-compilation targets win over the environment CXX.
ifeq ($(TARGET),rpi5)
    override CXX      := aarch64-linux-gnu-g++
    override CXXFLAGS := -std=c++17 -O3 -ffast-math -funroll-loops \
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

# In cross-compilation CXXFLAGS already contains -I$(STAGING_DIR)/usr/include
LV2FLAGS ?= $(shell pkg-config --cflags lv2 2>/dev/null)

BUNDLE  = megalo.lv2
BINARY  = $(BUNDLE)/megalo.so
SOURCES = src/plugin.cpp
HEADERS = src/freeze_engine.hpp src/granular_looper.hpp src/biquad.hpp src/envelope.hpp src/phase_vocoder.hpp

all: $(BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(EXTRA_DEFS) $(LV2FLAGS) -fPIC -shared -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(BINARY)

install: $(BINARY)
	install -d $(DESTDIR)/usr/lib/lv2/$(BUNDLE)
	cp $(BUNDLE)/*.so $(BUNDLE)/*.ttl $(DESTDIR)/usr/lib/lv2/$(BUNDLE)/

.PHONY: all clean install
