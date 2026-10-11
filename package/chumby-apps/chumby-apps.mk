################################################################################
#
# chumby-apps
#
################################################################################

CHUMBY_APPS_VERSION = 1.0
CHUMBY_APPS_SITE = $(BR2_EXTERNAL_PIFANSRF_PATH)/package/chumby-apps/src
CHUMBY_APPS_SITE_METHOD = local
CHUMBY_APPS_LICENSE = GPL-2.0+
CHUMBY_APPS_DEPENDENCIES = host-pkgconf libchumby libcurl mpg123 faad2 alsa-lib pixman

define CHUMBY_APPS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

# Each app: a binary plus a descriptor the shell lists on the home screen.
define CHUMBY_APPS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/chumby-clock $(TARGET_DIR)/usr/bin/chumby-clock
	$(INSTALL) -D -m 0755 $(@D)/chumby-fm $(TARGET_DIR)/usr/bin/chumby-fm
	$(INSTALL) -D -m 0755 $(@D)/chumby-stream $(TARGET_DIR)/usr/bin/chumby-stream
	$(INSTALL) -D -m 0755 $(@D)/chumby-calibrate $(TARGET_DIR)/usr/bin/chumby-calibrate
	$(INSTALL) -D -m 0755 $(@D)/chumby-airplay $(TARGET_DIR)/usr/bin/chumby-airplay
	$(INSTALL) -D -m 0755 $(@D)/chumby-airplay-meta $(TARGET_DIR)/usr/bin/chumby-airplay-meta
	$(INSTALL) -D -m 0755 $(@D)/chumby-airplay-remote $(TARGET_DIR)/usr/bin/chumby-airplay-remote
	$(INSTALL) -D -m 0755 $(@D)/chumby-toasters $(TARGET_DIR)/usr/lib/chumby/savers/toasters
	rm -f $(TARGET_DIR)/usr/bin/chumby-toasters
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/airplay.app \
		$(TARGET_DIR)/usr/share/chumby/apps/airplay.app
	$(INSTALL) -D -m 0755 $(CHUMBY_APPS_PKGDIR)/chumby-station-art \
		$(TARGET_DIR)/usr/bin/chumby-station-art
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/clock.app \
		$(TARGET_DIR)/usr/share/chumby/apps/clock.app
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/fm.app \
		$(TARGET_DIR)/usr/share/chumby/apps/fm.app
	$(INSTALL) -D -m 0755 $(@D)/chumby-settings $(TARGET_DIR)/usr/bin/chumby-settings
	$(INSTALL) -D -m 0644 $(CHUMBY_APPS_PKGDIR)/settings.app \
		$(TARGET_DIR)/usr/share/chumby/apps/settings.app
	$(INSTALL) -D -m 0755 $(CHUMBY_APPS_PKGDIR)/chumby-setting \
		$(TARGET_DIR)/usr/libexec/chumby-setting
	# Before Podcasts and Internet Radio were app bundles (left by an
	# older image's update: buildroot doesn't remove them).
	rm -f $(TARGET_DIR)/usr/bin/chumby-podcasts $(TARGET_DIR)/usr/bin/chumby-podcast-feed \
		$(TARGET_DIR)/usr/bin/chumby-netradio $(TARGET_DIR)/usr/share/chumby/apps/podcasts.app \
		$(TARGET_DIR)/usr/share/chumby/apps/netradio.app \
		$(TARGET_DIR)/usr/share/chumby/podcasts.conf $(TARGET_DIR)/usr/share/chumby/streams.conf
	$(INSTALL) -D -m 0755 $(CHUMBY_APPS_PKGDIR)/audio-stream \
		$(TARGET_DIR)/usr/share/chumby/audio.d/stream
endef

$(eval $(generic-package))
