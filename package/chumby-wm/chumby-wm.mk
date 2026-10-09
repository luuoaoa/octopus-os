################################################################################
#
# chumby-wm
#
################################################################################

CHUMBY_WM_VERSION = 1.0
CHUMBY_WM_SITE = $(BR2_EXTERNAL_PIFANSRF_PATH)/package/chumby-wm/src
CHUMBY_WM_SITE_METHOD = local
CHUMBY_WM_LICENSE = MIT
CHUMBY_WM_DEPENDENCIES = host-pkgconf host-wayland wayland-protocols \
	wlroots-pixman libxkbcommon pixman libinput

define CHUMBY_WM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		WAYLAND_SCANNER="$(HOST_DIR)/bin/wayland-scanner" \
		WAYLAND_PROTOCOLS="$(STAGING_DIR)/usr/share/wayland-protocols"
endef

define CHUMBY_WM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/chumby-wm $(TARGET_DIR)/usr/bin/chumby-wm
endef

$(eval $(generic-package))
