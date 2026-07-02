################################################################################
# megalohn — polyphonic harmonic+noise freeze/sustain LV2 effect
#
# Builds the MegaloHN plugin (distinct URI, cohabits with the stock Megalo)
# from the shared megalo_dsp core. Two MOD targets are supported and selected
# automatically from the active buildroot CPU profile:
#
#   • MOD Dwarf  (moddwarf-new, Cortex-A35) — constrained: granular pitch path,
#     conservative polyphony tier (dwarf: FFT 8192, 4 notes, 16 partials).
#   • MOD Duo X  (modduox-new,  Cortex-A53) — no Dwarf restrictions: full
#     features like the Pi 5 (phase vocoder + pi5 tier: FFT 16384, 6 notes,
#     32 partials).
#
# To update: set MEGALOHN_VERSION to the HEAD commit hash of the branch,
# then trigger a mod-plugin-builder rebuild.
################################################################################

MEGALOHN_VERSION = f44df2cf8495dbaf39d5fae0841d0c7895af4e9c
MEGALOHN_SITE    = $(call github,pilali,megalo,$(MEGALOHN_VERSION))
MEGALOHN_BUNDLES = megaloHN.lv2

# Per-platform build profile, keyed on the buildroot CPU. The MOD Duo X
# (Cortex-A53) and Pi 5 (Cortex-A76) carry no Dwarf restrictions, so they get
# the phase vocoder and the pi5-class analysis tier; the Dwarf (Cortex-A35)
# stays on the lean dwarf tier with the granular pitch path.
ifeq ($(BR2_cortex_a53),y)
MEGALOHN_TARGET = modduox-new
MEGALOHN_HN_Q   = pi5
else ifeq ($(BR2_cortex_a76),y)
MEGALOHN_TARGET = rpi5
MEGALOHN_HN_Q   = pi5
else
MEGALOHN_TARGET = moddwarf-new
MEGALOHN_HN_Q   = dwarf
endif

define MEGALOHN_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) megaloHN \
		TARGET=$(MEGALOHN_TARGET) \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -std=c++17 -O3 -ffast-math -fvisibility=hidden" \
		MEGALO_HN_QUALITY=$(MEGALOHN_HN_Q)
endef

define MEGALOHN_INSTALL_TARGET_CMDS
	cp -r $(@D)/megaloHN.lv2 $(TARGET_DIR)/usr/lib/lv2/
endef

$(eval $(generic-package))
