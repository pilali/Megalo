################################################################################
# megalo — LV2 freeze/sustain effect for MOD Dwarf / Raspberry Pi 5
#
# To update: set MEGALO_VERSION to the desired commit hash, then rebuild.
################################################################################

MEGALO_VERSION = 27277370e7707dc577bdc4fdf9b292f02d7a9530
MEGALO_SITE    = $(call github,pilali,megalo,$(MEGALO_VERSION))
MEGALO_BUNDLES = megalo.lv2

define MEGALO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -std=c++17 -O3 -ffast-math -fvisibility=hidden"
endef

define MEGALO_INSTALL_BUNDLES
	install -d $(TARGET_DIR)/usr/lib/lv2/megalo.lv2
	cp $(@D)/megalo.lv2/megalo.so \
	   $(@D)/megalo.lv2/manifest.ttl \
	   $(@D)/megalo.lv2/megalo.ttl \
	   $(TARGET_DIR)/usr/lib/lv2/megalo.lv2/
endef

$(eval $(generic-package))
