################################################################################
# megalo — LV2 freeze/sustain effect for MOD Dwarf / Raspberry Pi 5
#
# To update: set MEGALO_VERSION to the desired commit hash, then rebuild.
################################################################################

MEGALO_VERSION = b62fc7b0c7b91d8934ede5450f220f13f14aeaa1
MEGALO_SITE    = $(call github,pilali,megalo,$(MEGALO_VERSION))
MEGALO_BUNDLES = megalo.lv2

define MEGALO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -std=c++17 -O3 -ffast-math -fvisibility=hidden"
endef

define MEGALO_INSTALL_TARGET_CMDS
	cp -r $(@D)/megalo.lv2 $(TARGET_DIR)/usr/lib/lv2/
endef

$(eval $(generic-package))
