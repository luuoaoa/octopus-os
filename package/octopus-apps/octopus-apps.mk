################################################################################
#
# octopus-apps
#
################################################################################

OCTOPUS_APPS_VERSION = 1.0
OCTOPUS_APPS_SITE = $(BR2_EXTERNAL_OCTOPUS_PATH)/package/octopus-apps/src
OCTOPUS_APPS_SITE_METHOD = local
OCTOPUS_APPS_LICENSE = GPL-2.0+
OCTOPUS_APPS_DEPENDENCIES = host-pkgconf liboctopus libcurl mpg123 faad2 alsa-lib pixman

define OCTOPUS_APPS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

# Each app: a binary plus a descriptor the shell lists on the home screen.
define OCTOPUS_APPS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/octopus-clock $(TARGET_DIR)/usr/bin/octopus-clock
	$(INSTALL) -D -m 0755 $(@D)/octopus-fm $(TARGET_DIR)/usr/bin/octopus-fm
	$(INSTALL) -D -m 0755 $(@D)/octopus-stream $(TARGET_DIR)/usr/bin/octopus-stream
	$(INSTALL) -D -m 0755 $(@D)/octopus-calibrate $(TARGET_DIR)/usr/bin/octopus-calibrate
	$(INSTALL) -D -m 0755 $(@D)/octopus-airplay $(TARGET_DIR)/usr/bin/octopus-airplay
	$(INSTALL) -D -m 0755 $(@D)/octopus-airplay-meta $(TARGET_DIR)/usr/bin/octopus-airplay-meta
	$(INSTALL) -D -m 0755 $(@D)/octopus-airplay-remote $(TARGET_DIR)/usr/bin/octopus-airplay-remote
	$(INSTALL) -D -m 0755 $(@D)/octopus-toasters $(TARGET_DIR)/usr/lib/octopus/savers/toasters
	rm -f $(TARGET_DIR)/usr/bin/octopus-toasters
	$(INSTALL) -D -m 0644 $(OCTOPUS_APPS_PKGDIR)/airplay.app \
		$(TARGET_DIR)/usr/share/octopus/apps/airplay.app
	$(INSTALL) -D -m 0755 $(OCTOPUS_APPS_PKGDIR)/octopus-station-art \
		$(TARGET_DIR)/usr/bin/octopus-station-art
	$(INSTALL) -D -m 0644 $(OCTOPUS_APPS_PKGDIR)/clock.app \
		$(TARGET_DIR)/usr/share/octopus/apps/clock.app
	$(INSTALL) -D -m 0644 $(OCTOPUS_APPS_PKGDIR)/fm.app \
		$(TARGET_DIR)/usr/share/octopus/apps/fm.app
	$(INSTALL) -D -m 0755 $(@D)/octopus-settings $(TARGET_DIR)/usr/bin/octopus-settings
	$(INSTALL) -D -m 0644 $(OCTOPUS_APPS_PKGDIR)/settings.app \
		$(TARGET_DIR)/usr/share/octopus/apps/settings.app
	$(INSTALL) -D -m 0755 $(OCTOPUS_APPS_PKGDIR)/octopus-setting \
		$(TARGET_DIR)/usr/libexec/octopus-setting
	# Before Podcasts and Internet Radio were app bundles (left by an
	# older image's update: buildroot doesn't remove them).
	rm -f $(TARGET_DIR)/usr/bin/octopus-podcasts $(TARGET_DIR)/usr/bin/octopus-podcast-feed \
		$(TARGET_DIR)/usr/bin/octopus-netradio $(TARGET_DIR)/usr/share/octopus/apps/podcasts.app \
		$(TARGET_DIR)/usr/share/octopus/apps/netradio.app \
		$(TARGET_DIR)/usr/share/octopus/podcasts.conf $(TARGET_DIR)/usr/share/octopus/streams.conf
	$(INSTALL) -D -m 0755 $(OCTOPUS_APPS_PKGDIR)/audio-stream \
		$(TARGET_DIR)/usr/share/octopus/audio.d/stream
endef

$(eval $(generic-package))
