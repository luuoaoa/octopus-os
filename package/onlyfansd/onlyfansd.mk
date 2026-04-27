################################################################################
#
# onlyfansd
#
# Pinned at a specific commit of luuoaoa/ceiling-fans-control. Buildroot's
# cargo download post-process handles `cargo vendor` + .cargo/config.toml
# generation at fetch time, so the pinned tarball doesn't need to ship
# pre-vendored deps.
#
################################################################################

ONLYFANSD_VERSION = ebbe4eb65e1415a10e726271df69f8ee71cefe92
ONLYFANSD_SITE = $(call github,luuoaoa,ceiling-fans-control,$(ONLYFANSD_VERSION))
ONLYFANSD_LICENSE = MIT
ONLYFANSD_DEPENDENCIES = librpitx

# build.rs / cc crate need the cross C++ compiler and the librpitx staging
# paths. We export both unprefixed (CC/CXX/AR — picked up via TARGET_CC
# fallback) and triple-prefixed forms (CC_<triple_with_underscores> —
# preferred by the cc crate for cross builds) so it never picks up the
# host compiler. We also pin the cross sysroot via *FLAGS so that the
# shim sees librpitx headers from staging.
ONLYFANSD_CARGO_ENV = \
	LIBRPITX_INCLUDE_DIR=$(STAGING_DIR)/usr/include \
	LIBRPITX_LIB_DIR=$(STAGING_DIR)/usr/lib \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)" \
	CFLAGS="$(TARGET_CFLAGS)" \
	CXXFLAGS="$(TARGET_CXXFLAGS)" \
	CC_$(subst -,_,$(GNU_TARGET_NAME))="$(TARGET_CC)" \
	CXX_$(subst -,_,$(GNU_TARGET_NAME))="$(TARGET_CXX)" \
	AR_$(subst -,_,$(GNU_TARGET_NAME))="$(TARGET_AR)"

define ONLYFANSD_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(ONLYFANSD_PKGDIR)/onlyfansd.service \
		$(TARGET_DIR)/usr/lib/systemd/system/onlyfansd.service
endef

# Stage the sample config under /usr/share so the user can copy it to
# /boot/onlyfansd.yaml from the running Pi. /etc/onlyfansd lives in the
# in-RAM rootfs (cpio) so writing there doesn't persist; /var/lib is
# created at runtime by systemd's StateDirectory=.
define ONLYFANSD_INSTALL_TARGET_CONF
	$(INSTALL) -D -m 0644 $(@D)/config_sample.yaml \
		$(TARGET_DIR)/usr/share/onlyfansd/config.yaml.example
endef

ONLYFANSD_POST_INSTALL_TARGET_HOOKS += ONLYFANSD_INSTALL_TARGET_CONF

$(eval $(cargo-package))
