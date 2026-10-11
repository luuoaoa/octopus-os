################################################################################
#
# chumby-bundled-apps
#
################################################################################

# Every app in apps/ (a directory with a Makefile and an app.conf, as the
# SDK makes them), built as a bundle and preinstalled read-only in
# /usr/lib/chumby/apps/<Id>.
CHUMBY_BUNDLED_APPS_VERSION = 1.0
CHUMBY_BUNDLED_APPS_SITE = $(BR2_EXTERNAL_PIFANSRF_PATH)/apps
CHUMBY_BUNDLED_APPS_SITE_METHOD = local
CHUMBY_BUNDLED_APPS_LICENSE = GPL-2.0+
CHUMBY_BUNDLED_APPS_DEPENDENCIES = host-pkgconf libchumby libcurl expat

define CHUMBY_BUNDLED_APPS_BUILD_CMDS
	for app in $(@D)/*/; do \
		[ -f $$app/Makefile ] || continue; \
		$(TARGET_MAKE_ENV) $(MAKE) -C $$app \
			CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
			PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" || exit 1; \
	done
endef

define CHUMBY_BUNDLED_APPS_INSTALL_TARGET_CMDS
	rm -rf $(TARGET_DIR)/usr/lib/chumby/apps
	for app in $(@D)/*/; do \
		[ -f $$app/bundle/app.conf ] || continue; \
		id=$$(sed -n 's/^[[:space:]]*Id[[:space:]]*=[[:space:]]*//p' $$app/bundle/app.conf | head -1); \
		mkdir -p $(TARGET_DIR)/usr/lib/chumby/apps/$$id; \
		cp -a $$app/bundle/. $(TARGET_DIR)/usr/lib/chumby/apps/$$id/ || exit 1; \
	done
endef

$(eval $(generic-package))
