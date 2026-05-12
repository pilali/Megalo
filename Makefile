CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -ffast-math -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
LV2FLAGS  = $(shell pkg-config --cflags lv2)

BUNDLE  = megalo.lv2
BINARY  = $(BUNDLE)/megalo.so
SOURCES = src/plugin.cpp
HEADERS = src/freeze_engine.hpp src/biquad.hpp src/envelope.hpp

all: $(BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(LV2FLAGS) -fPIC -shared -o $@ $(SOURCES)

clean:
	rm -f $(BINARY)

install: $(BINARY)
	install -d $(DESTDIR)/usr/lib/lv2/$(BUNDLE)
	cp $(BUNDLE)/*.so $(BUNDLE)/*.ttl $(DESTDIR)/usr/lib/lv2/$(BUNDLE)/

.PHONY: all clean install
