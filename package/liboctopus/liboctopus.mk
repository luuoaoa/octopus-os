################################################################################
#
# liboctopus
#
################################################################################

LIBOCTOPUS_VERSION = 1.0
LIBOCTOPUS_SITE = $(BR2_EXTERNAL_OCTOPUS_PATH)/package/liboctopus/src
LIBOCTOPUS_SITE_METHOD = local
LIBOCTOPUS_LICENSE = GPL-2.0+ (framework, Nimbus fonts from Rockbox), MIT (LVGL)
LIBOCTOPUS_INSTALL_STAGING = YES
LIBOCTOPUS_DEPENDENCIES = host-pkgconf host-wayland wayland wayland-protocols \
	libxkbcommon alsa-lib lvgl pixman

# LVGL is compiled into the library with our lv_conf.h. (A local package
# never runs the download step, so LVGL comes from its own package.)
define LIBOCTOPUS_BUILD_CMDS
	rsync -a --delete $(LVGL_DIR)/ $(@D)/lvgl/
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		WAYLAND_SCANNER="$(HOST_DIR)/bin/wayland-scanner" \
		WAYLAND_PROTOCOLS="$(STAGING_DIR)/usr/share/wayland-protocols"
endef

# Headers: octopus.h, octopus_saver.h (the screen saver kit), lv_conf.h and LVGL's headers under include/octopus/.
define LIBOCTOPUS_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0755 $(@D)/liboctopus.so.1 $(STAGING_DIR)/usr/lib/liboctopus.so.1
	ln -sf liboctopus.so.1 $(STAGING_DIR)/usr/lib/liboctopus.so
	mkdir -p $(STAGING_DIR)/usr/include/octopus
	$(INSTALL) -m 0644 $(@D)/octopus.h $(@D)/octopus_saver.h $(@D)/lv_conf.h \
		$(STAGING_DIR)/usr/include/octopus/
	rsync -a --include '*/' --include '*.h' --exclude '*' \
		$(@D)/lvgl/ $(STAGING_DIR)/usr/include/octopus/lvgl/
	$(INSTALL) -D -m 0644 $(LIBOCTOPUS_PKGDIR)/liboctopus.pc \
		$(STAGING_DIR)/usr/lib/pkgconfig/liboctopus.pc
endef

define LIBOCTOPUS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/liboctopus.so.1 $(TARGET_DIR)/usr/lib/liboctopus.so.1
	$(INSTALL) -D -m 0644 $(LIBOCTOPUS_PKGDIR)/policy.conf \
		$(TARGET_DIR)/etc/octopus/policy.conf
endef

$(eval $(generic-package))
