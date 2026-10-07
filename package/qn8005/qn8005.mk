################################################################################
#
# qn8005
#
################################################################################

QN8005_VERSION = 1.0
QN8005_SITE = $(BR2_EXTERNAL_PIFANSRF_PATH)/package/qn8005/src
QN8005_SITE_METHOD = local
QN8005_LICENSE = GPL-2.0+

define QN8005_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/qn8005 $(@D)/qn8005.c
endef

define QN8005_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/qn8005 $(TARGET_DIR)/usr/bin/qn8005
endef

$(eval $(generic-package))
