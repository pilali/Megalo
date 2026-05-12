################################################################################
# megalo — LV2 freeze/sustain effect for MOD Dwarf
################################################################################

MEGALO_VERSION = main
MEGALO_SITE    = https://github.com/pilali/megalo.git
MEGALO_SITE_METHOD = git
MEGALO_BUNDLES = megalo.lv2

define MEGALO_BUILD_CMDS
	$(TARGET_MAKE_ENV) cmake $(@D) \
		-DCMAKE_TOOLCHAIN_FILE="$(HOST_DIR)/usr/share/buildroot/toolchainfile.cmake" \
		-DCMAKE_BUILD_TYPE=Release \
		-B$(@D)/build
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/build
endef

define MEGALO_INSTALL_BUNDLES
	cp -r $(@D)/build/megalo.lv2/. $(TARGET_DIR)/usr/lib/lv2/megalo.lv2/
endef

$(eval $(generic-package))
