################################################################################
#
# chumby-ui
#
################################################################################

CHUMBY_UI_VERSION = 1.0
CHUMBY_UI_SITE = $(BR2_EXTERNAL_PIFANSRF_PATH)/package/chumby-ui/src
CHUMBY_UI_SITE_METHOD = local
CHUMBY_UI_LICENSE = GPL-2.0+
CHUMBY_UI_DEPENDENCIES = alsa-lib freetype host-pkgconf lvgl

# LVGL is compiled straight into the app with our lv_conf.h. (A local
# package never runs the download step, so LVGL comes from its own
# source-only package rather than CHUMBY_UI_EXTRA_DOWNLOADS.)
define CHUMBY_UI_BUILD_CMDS
	rsync -a --delete $(LVGL_DIR)/ $(@D)/lvgl/
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		FT_CFLAGS="`$(PKG_CONFIG_HOST_BINARY) --cflags freetype2`" \
		FT_LIBS="`$(PKG_CONFIG_HOST_BINARY) --libs freetype2`"
endef

define CHUMBY_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/chumby-ui $(TARGET_DIR)/usr/bin/chumby-ui
endef

define CHUMBY_UI_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(CHUMBY_UI_PKGDIR)/S95chumby-ui \
		$(TARGET_DIR)/etc/init.d/S95chumby-ui
endef

$(eval $(generic-package))
