################################################################################
# megalo — LV2 freeze/sustain effect for MOD Dwarf / Raspberry Pi 5
#
# Branch : experimental/additive-synth
# To update: set MEGALO_VERSION to the HEAD commit hash of the branch,
# then trigger a mod-plugin-builder rebuild.
################################################################################

MEGALO_VERSION = bf945316fd9ccb7181c63c054921a61a449ada1a
MEGALO_SITE    = $(call github,pilali,megalo,$(MEGALO_VERSION))
MEGALO_BUNDLES = megaloHN.lv2

define MEGALO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		TARGET=moddwarf-new \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -std=c++17 -O3 -ffast-math -fvisibility=hidden" \
		MEGALO_HN_SYNTH=1
endef

define MEGALO_INSTALL_TARGET_CMDS
	cp -r $(@D)/megalo.lv2 $(TARGET_DIR)/usr/lib/lv2/
endef

$(eval $(generic-package))
