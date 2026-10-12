################################################################################
#
# octopus-wm
#
################################################################################

OCTOPUS_WM_VERSION = 1.0
OCTOPUS_WM_SITE = $(BR2_EXTERNAL_OCTOPUS_PATH)/package/octopus-wm/src
OCTOPUS_WM_SITE_METHOD = local
OCTOPUS_WM_LICENSE = MIT
OCTOPUS_WM_DEPENDENCIES = host-pkgconf host-wayland wayland-protocols \
	wlroots-pixman libxkbcommon pixman libinput

define OCTOPUS_WM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		WAYLAND_SCANNER="$(HOST_DIR)/bin/wayland-scanner" \
		WAYLAND_PROTOCOLS="$(STAGING_DIR)/usr/share/wayland-protocols"
endef

define OCTOPUS_WM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/octopus-wm $(TARGET_DIR)/usr/bin/octopus-wm
endef

$(eval $(generic-package))
