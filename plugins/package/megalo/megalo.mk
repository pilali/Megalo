################################################################################
# megalo — LV2 freeze/sustain effect for MOD Dwarf
################################################################################

MEGALO_VERSION = main
MEGALO_SITE    = https://github.com/pilali/megalo.git
MEGALO_SITE_METHOD = git
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
