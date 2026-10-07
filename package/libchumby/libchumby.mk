################################################################################
#
# libchumby
#
################################################################################

LIBCHUMBY_VERSION = 1.0
LIBCHUMBY_SITE = $(BR2_EXTERNAL_PIFANSRF_PATH)/package/libchumby/src
LIBCHUMBY_SITE_METHOD = local
LIBCHUMBY_LICENSE = GPL-2.0+ (framework), MIT (LVGL)
LIBCHUMBY_INSTALL_STAGING = YES
LIBCHUMBY_DEPENDENCIES = host-pkgconf host-wayland wayland wayland-protocols \
	libxkbcommon freetype alsa-lib lvgl

# LVGL is compiled into the library with our lv_conf.h. (A local package
# never runs the download step, so LVGL comes from its own package.)
define LIBCHUMBY_BUILD_CMDS
	rsync -a --delete $(LVGL_DIR)/ $(@D)/lvgl/
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		WAYLAND_SCANNER="$(HOST_DIR)/bin/wayland-scanner" \
		WAYLAND_PROTOCOLS="$(STAGING_DIR)/usr/share/wayland-protocols"
endef

# Headers: chumby.h, lv_conf.h and LVGL's headers under include/chumby/.
define LIBCHUMBY_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libchumby.so.1 $(STAGING_DIR)/usr/lib/libchumby.so.1
	ln -sf libchumby.so.1 $(STAGING_DIR)/usr/lib/libchumby.so
	mkdir -p $(STAGING_DIR)/usr/include/chumby
	$(INSTALL) -m 0644 $(@D)/chumby.h $(@D)/lv_conf.h $(STAGING_DIR)/usr/include/chumby/
	rsync -a --include '*/' --include '*.h' --exclude '*' \
		$(@D)/lvgl/ $(STAGING_DIR)/usr/include/chumby/lvgl/
	$(INSTALL) -D -m 0644 $(LIBCHUMBY_PKGDIR)/libchumby.pc \
		$(STAGING_DIR)/usr/lib/pkgconfig/libchumby.pc
endef

define LIBCHUMBY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libchumby.so.1 $(TARGET_DIR)/usr/lib/libchumby.so.1
	$(INSTALL) -D -m 0644 $(LIBCHUMBY_PKGDIR)/policy.conf \
		$(TARGET_DIR)/etc/chumby/policy.conf
endef

$(eval $(generic-package))
