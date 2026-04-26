################################################################################
#
# rpitx (minimal: only targets that need librpitx alone — no sndfile, no ft8)
#
################################################################################

RPITX_VERSION = ee7ff57b77962536fda4daa523749c04af6beec7
RPITX_SITE = https://github.com/F5OEO/rpitx.git
RPITX_SITE_METHOD = git
RPITX_LICENSE = GPL-3.0
RPITX_LICENSE_FILES = LICENCE
RPITX_DEPENDENCIES = librpitx

# rpitx's src/Makefile reads CXX/CC/CFLAGS/CXXFLAGS/LDFLAGS from env. Buildroot's
# TARGET_CXXFLAGS already includes -I $(STAGING_DIR)/usr/include; TARGET_LDFLAGS
# includes -L $(STAGING_DIR)/usr/lib. The Makefile appends `-lrpitx -lm -lrt
# -lpthread` to LDFLAGS itself.
RPITX_MAKE_OPTS = \
	CXX="$(TARGET_CXX)" \
	CC="$(TARGET_CC)" \
	CXXFLAGS="$(TARGET_CXXFLAGS) -Wno-write-strings -Wno-unused-variable" \
	CFLAGS="$(TARGET_CFLAGS) -Wno-unused-variable" \
	LDFLAGS="$(TARGET_LDFLAGS)"

# We hand-pick targets so we don't drag in libsndfile / ft8_lib / csdr.
RPITX_BIN_TARGETS = ../tune ../sendook ../sendiq ../morse ../rpitx

define RPITX_BUILD_CMDS
	$(MAKE1) -C $(@D)/src $(RPITX_MAKE_OPTS) $(RPITX_BIN_TARGETS)
endef

define RPITX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tune    $(TARGET_DIR)/usr/bin/tune
	$(INSTALL) -D -m 0755 $(@D)/sendook $(TARGET_DIR)/usr/bin/sendook
	$(INSTALL) -D -m 0755 $(@D)/sendiq  $(TARGET_DIR)/usr/bin/sendiq
	$(INSTALL) -D -m 0755 $(@D)/morse   $(TARGET_DIR)/usr/bin/morse
	$(INSTALL) -D -m 0755 $(@D)/rpitx   $(TARGET_DIR)/usr/bin/rpitx-tool
endef

$(eval $(generic-package))
