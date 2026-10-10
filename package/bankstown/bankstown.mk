################################################################################
#
# bankstown — LV2 bass enhancement plugin (cdylib + .ttl manifest)
#
# Pinned at a specific commit of chadmed/bankstown. Buildroot's cargo
# download post-process handles `cargo vendor` + .cargo/config.toml
# generation at fetch time, so the pinned tarball doesn't need to ship
# pre-vendored deps. Same model as onlyfansd / briefingd / etc.
#
################################################################################

BANKSTOWN_VERSION = e9829c9bccf5ed73768135c0ddd506f5a6690f9e
BANKSTOWN_SITE = $(call github,chadmed,bankstown,$(BANKSTOWN_VERSION))
BANKSTOWN_LICENSE = MIT
BANKSTOWN_LICENSE_FILES = LICENSE

BANKSTOWN_CARGO_ENV = \
	CC="$(TARGET_CC)" \
	AR="$(TARGET_AR)" \
	CFLAGS="$(TARGET_CFLAGS)" \
	CC_$(subst -,_,$(GNU_TARGET_NAME))="$(TARGET_CC)" \
	AR_$(subst -,_,$(GNU_TARGET_NAME))="$(TARGET_AR)"

# cargo-package's default install runs `cargo install --bins`, which
# does nothing for a cdylib. Override to copy the .so + LV2 .ttl
# manifests into /usr/lib/lv2/bankstown.lv2/ — same layout the
# upstream Makefile produces.
define BANKSTOWN_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/usr/lib/lv2/bankstown.lv2
	$(INSTALL) -m 0755 $(@D)/target/$(RUSTC_TARGET_NAME)/release/libbankstown.so \
		$(TARGET_DIR)/usr/lib/lv2/bankstown.lv2/bankstown.so
	$(INSTALL) -m 0644 $(@D)/bankstown.ttl \
		$(TARGET_DIR)/usr/lib/lv2/bankstown.lv2/bankstown.ttl
	$(INSTALL) -m 0644 $(@D)/manifest.ttl \
		$(TARGET_DIR)/usr/lib/lv2/bankstown.lv2/manifest.ttl
endef

$(eval $(cargo-package))
