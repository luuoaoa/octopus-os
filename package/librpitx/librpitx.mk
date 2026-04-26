################################################################################
#
# librpitx
#
################################################################################

LIBRPITX_VERSION = f01bdb64bcdb6207f448379193bc0a8accb9aa22
LIBRPITX_SITE = https://github.com/F5OEO/librpitx.git
LIBRPITX_SITE_METHOD = git
LIBRPITX_LICENSE = GPL-3.0
LIBRPITX_LICENSE_FILES = LICENCE.txt
LIBRPITX_INSTALL_STAGING = YES
LIBRPITX_INSTALL_TARGET = NO

# librpitx provides its own bcm_host_get_*_address() in rpi.c, so the
# -lbcm_host link flag and the -I/-L /opt/vc paths are unused. Override
# LDFLAGS / CXXFLAGS to drop them — that lets us build outside of a
# Raspbian sysroot with /opt/vc.
LIBRPITX_MAKE_OPTS = \
	CXX="$(TARGET_CXX)" \
	CC="$(TARGET_CC)" \
	AR="$(TARGET_AR)" \
	CXXFLAGS="$(TARGET_CXXFLAGS) -fPIC" \
	CFLAGS="$(TARGET_CFLAGS) -fPIC" \
	LDFLAGS="-lm -lrt -lpthread -fPIC"

define LIBRPITX_BUILD_CMDS
	$(MAKE1) $(LIBRPITX_MAKE_OPTS) -C $(@D)/src
endef

define LIBRPITX_INSTALL_STAGING_CMDS
	$(MAKE1) $(LIBRPITX_MAKE_OPTS) PREFIX=$(STAGING_DIR)/usr \
		-C $(@D)/src install
endef

$(eval $(generic-package))
