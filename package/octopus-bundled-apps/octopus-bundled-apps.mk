################################################################################
#
# octopus-bundled-apps
#
################################################################################

# Every app in apps/ (a directory with a Makefile and an app.conf, as the
# SDK makes them), built as a bundle and preinstalled read-only in
# /usr/lib/octopus/apps/<Id>.
OCTOPUS_BUNDLED_APPS_VERSION = 1.0
OCTOPUS_BUNDLED_APPS_SITE = $(BR2_EXTERNAL_OCTOPUS_PATH)/apps
OCTOPUS_BUNDLED_APPS_SITE_METHOD = local
OCTOPUS_BUNDLED_APPS_LICENSE = GPL-2.0+
OCTOPUS_BUNDLED_APPS_DEPENDENCIES = host-pkgconf liboctopus libcurl expat

define OCTOPUS_BUNDLED_APPS_BUILD_CMDS
	for app in $(@D)/*/; do \
		[ -f $$app/Makefile ] || continue; \
		$(TARGET_MAKE_ENV) $(MAKE) -C $$app \
			CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
			PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" || exit 1; \
	done
endef

define OCTOPUS_BUNDLED_APPS_INSTALL_TARGET_CMDS
	rm -rf $(TARGET_DIR)/usr/lib/octopus/apps
	for app in $(@D)/*/; do \
		[ -f $$app/bundle/app.conf ] || continue; \
		id=$$(sed -n 's/^[[:space:]]*Id[[:space:]]*=[[:space:]]*//p' $$app/bundle/app.conf | head -1); \
		mkdir -p $(TARGET_DIR)/usr/lib/octopus/apps/$$id; \
		cp -a $$app/bundle/. $(TARGET_DIR)/usr/lib/octopus/apps/$$id/ || exit 1; \
	done
endef

$(eval $(generic-package))
