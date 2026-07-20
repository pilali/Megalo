################################################################################
# megalo — granular freeze/sustain LV2 effect for MOD Dwarf / Raspberry Pi 5
#
# Builds the stock Megalo plugin (granular pitch path, MEGALO_HN_SYNTH off) from
# the shared megalo_dsp core. Its sibling MegaloHN (polyphonic harmonic+noise)
# is packaged separately under plugins/package/megalohn.
#
# To update: set MEGALO_VERSION to the desired commit hash, then rebuild.
################################################################################

MEGALO_VERSION = 484cda5a1d77bf879e85f79c0bfd7857aa75bd19
MEGALO_SITE    = $(call github,pilali,megalo,$(MEGALO_VERSION))
MEGALO_BUNDLES = megalo.lv2

# Enable PhaseVocoder pitch shifter on RPi5 (Cortex-A76 / ARMv8.2-A).
# On MOD Dwarf (Cortex-A35) the GrainPlayer fallback is used instead.
# The defines are passed as a command-line EXTRA_DEFS override — appending
# them to CXXFLAGS instead would clash with the moddwarf-new target's own
# EXTRA_DEFS (-DMEGALO_PV_N=1024), which comes later on the compile line and
# silently knocked the RPi5 vocoder down to a 1024-point FFT.
ifeq ($(BR2_cortex_a76),y)
MEGALO_PV_OVERRIDE = EXTRA_DEFS="-DMEGALO_PHASE_VOCODER -DMEGALO_PV_N=2048"
else
MEGALO_PV_OVERRIDE =
endif

# `megalo` target builds only the stock bundle (the Makefile's default `all`
# would also build megaloHN, which is packaged separately).
define MEGALO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) megalo \
		TARGET=moddwarf-new \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -std=c++17 -O3 -ffast-math -fvisibility=hidden" \
		$(MEGALO_PV_OVERRIDE)
endef

define MEGALO_INSTALL_TARGET_CMDS
	cp -r $(@D)/megalo.lv2 $(TARGET_DIR)/usr/lib/lv2/
endef

$(eval $(generic-package))
