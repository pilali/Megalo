################################################################################
# megaloHN — LV2 freeze/sustain + H+N additive synthesis for MOD Dwarf / RPi5
#
# To update: set MEGALO_VERSION to the desired commit hash, then rebuild.
################################################################################

MEGALO_VERSION = 58cfce2fc8a10059ea0f96a20fbde6c77abeecc9
MEGALO_SITE    = $(call github,pilali,megalo,$(MEGALO_VERSION))
MEGALO_BUNDLES = megaloHN.lv2

# Enable PhaseVocoder pitch shifter on RPi5 (Cortex-A76 / ARMv8.2-A).
# On MOD Dwarf (Cortex-A35) the GrainPlayer / HN-synth fallback is used.
ifeq ($(BR2_cortex_a76),y)
MEGALO_PV_DEFS = -DMEGALO_PHASE_VOCODER -DMEGALO_PV_N=2048
else
MEGALO_PV_DEFS =
endif

define MEGALO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		TARGET=moddwarf-new \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -std=c++17 -O3 -ffast-math -fvisibility=hidden $(MEGALO_PV_DEFS)" \
		MEGALO_HN_SYNTH=1
endef

define MEGALO_INSTALL_TARGET_CMDS
	cp -r $(@D)/megaloHN.lv2 $(TARGET_DIR)/usr/lib/lv2/
endef

$(eval $(generic-package))
