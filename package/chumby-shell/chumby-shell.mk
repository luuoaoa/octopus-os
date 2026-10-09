################################################################################
#
# chumby-shell
#
################################################################################

CHUMBY_SHELL_VERSION = 1.0
CHUMBY_SHELL_SITE = $(BR2_EXTERNAL_PIFANSRF_PATH)/package/chumby-shell/src
CHUMBY_SHELL_SITE_METHOD = local
CHUMBY_SHELL_LICENSE = GPL-2.0+
CHUMBY_SHELL_DEPENDENCIES = host-pkgconf host-wayland alsa-lib libchumby wayland

define CHUMBY_SHELL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		WAYLAND_SCANNER="$(HOST_DIR)/bin/wayland-scanner"
endef

define CHUMBY_SHELL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/chumby-shell $(TARGET_DIR)/usr/bin/chumby-shell
	$(INSTALL) -D -m 0755 $(@D)/chumby-alarm-tone $(TARGET_DIR)/usr/bin/chumby-alarm-tone
	$(INSTALL) -D -m 0755 $(@D)/chumby-screenshot $(TARGET_DIR)/usr/bin/chumby-screenshot
endef

# Starts chumby-wm (which starts the shell) once the display is up.
define CHUMBY_SHELL_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(CHUMBY_SHELL_PKGDIR)/S95chumby-session \
		$(TARGET_DIR)/etc/init.d/S95chumby-session
endef

define CHUMBY_SHELL_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(CHUMBY_SHELL_PKGDIR)/chumby-session.service \
		$(TARGET_DIR)/usr/lib/systemd/system/chumby-session.service
endef

$(eval $(generic-package))
