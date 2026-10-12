################################################################################
#
# octopus-shell
#
################################################################################

OCTOPUS_SHELL_VERSION = 1.0
OCTOPUS_SHELL_SITE = $(BR2_EXTERNAL_OCTOPUS_PATH)/package/octopus-shell/src
OCTOPUS_SHELL_SITE_METHOD = local
OCTOPUS_SHELL_LICENSE = GPL-2.0+
OCTOPUS_SHELL_DEPENDENCIES = host-pkgconf host-wayland alsa-lib liboctopus wayland

define OCTOPUS_SHELL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		WAYLAND_SCANNER="$(HOST_DIR)/bin/wayland-scanner"
endef

define OCTOPUS_SHELL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/octopus-shell $(TARGET_DIR)/usr/bin/octopus-shell
	$(INSTALL) -D -m 0755 $(@D)/octopus-alarm-tone $(TARGET_DIR)/usr/bin/octopus-alarm-tone
	$(INSTALL) -D -m 0755 $(@D)/octopus-screenshot $(TARGET_DIR)/usr/bin/octopus-screenshot
endef

# Starts octopus-wm (which starts the shell) once the display is up.
define OCTOPUS_SHELL_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(OCTOPUS_SHELL_PKGDIR)/S95octopus-session \
		$(TARGET_DIR)/etc/init.d/S95octopus-session
endef

define OCTOPUS_SHELL_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(OCTOPUS_SHELL_PKGDIR)/octopus-session.service \
		$(TARGET_DIR)/usr/lib/systemd/system/octopus-session.service
endef

$(eval $(generic-package))
