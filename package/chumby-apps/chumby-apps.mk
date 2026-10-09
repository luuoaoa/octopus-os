################################################################################
#
# chumby-apps
#
################################################################################

CHUMBY_APPS_VERSION = 1.0
CHUMBY_APPS_SITE = $(BR2_EXTERNAL_PIFANSRF_PATH)/package/chumby-apps/src
CHUMBY_APPS_SITE_METHOD = local
CHUMBY_APPS_LICENSE = GPL-2.0+
CHUMBY_APPS_DEPENDENCIES = host-pkgconf libchumby libcurl mpg123 faad2 alsa-lib

define CHUMBY_APPS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

# Each app: a binary plus a descriptor the shell lists on the home screen.
define CHUMBY_APPS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/chumby-clock $(TARGET_DIR)/usr/bin/chumby-clock
	$(INSTALL) -D -m 0755 $(@D)/chumby-fm $(TARGET_DIR)/usr/bin/chumby-fm
	$(INSTALL) -D -m 0755 $(@D)/chumby-netradio $(TARGET_DIR)/usr/bin/chumby-netradio
	$(INSTALL) -D -m 0755 $(@D)/chumby-stream $(TARGET_DIR)/usr/bin/chumby-stream
	$(INSTALL) -D -m 0755 $(@D)/chumby-calibrate $(TARGET_DIR)/usr/bin/chumby-calibrate
	$(INSTALL) -D -m 0755 $(CHUMBY_APPS_PKGDIR)/chumby-station-art \
		$(TARGET_DIR)/usr/bin/chumby-station-art
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/clock.app \
		$(TARGET_DIR)/usr/share/chumby/apps/clock.app
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/fm.app \
		$(TARGET_DIR)/usr/share/chumby/apps/fm.app
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/netradio.app \
		$(TARGET_DIR)/usr/share/chumby/apps/netradio.app
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/calibrate.app \
		$(TARGET_DIR)/usr/share/chumby/apps/calibrate.app
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/streams.conf \
		$(TARGET_DIR)/usr/share/chumby/streams.conf
endef

$(eval $(generic-package))
