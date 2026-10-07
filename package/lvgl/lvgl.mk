################################################################################
#
# lvgl
#
################################################################################

# Source-only package: LVGL is compiled into each app together with that
# app's own lv_conf.h (see package/chumby-ui), so nothing is built or
# installed here.
LVGL_VERSION = 9.6.0
LVGL_SITE = $(call github,lvgl,lvgl,v$(LVGL_VERSION))
LVGL_LICENSE = MIT
LVGL_LICENSE_FILES = LICENCE.txt
LVGL_INSTALL_TARGET = NO

$(eval $(generic-package))
