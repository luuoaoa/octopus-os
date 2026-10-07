################################################################################
#
# font-noto-sans
#
################################################################################

# Hinted static TTFs (better at small sizes on a low-DPI panel than the
# variable font) from the Noto fonts site repository, pinned to a commit.
# Only these two files are fetched; buildroot's googlefontdirectory would
# download the whole google/fonts repository.
FONT_NOTO_SANS_VERSION = 28b15b4b43b7bed62b5cf6e6b0b5ff5846270535
FONT_NOTO_SANS_SITE = https://raw.githubusercontent.com/notofonts/notofonts.github.io/$(FONT_NOTO_SANS_VERSION)/fonts/NotoSans/hinted/ttf
FONT_NOTO_SANS_SOURCE = NotoSans-Regular.ttf
FONT_NOTO_SANS_EXTRA_DOWNLOADS = \
	NotoSans-Bold.ttf \
	https://raw.githubusercontent.com/notofonts/notofonts.github.io/$(FONT_NOTO_SANS_VERSION)/fonts/LICENSE
FONT_NOTO_SANS_LICENSE = OFL-1.1
FONT_NOTO_SANS_LICENSE_FILES = LICENSE

define FONT_NOTO_SANS_EXTRACT_CMDS
	cp $(FONT_NOTO_SANS_DL_DIR)/NotoSans-Regular.ttf \
		$(FONT_NOTO_SANS_DL_DIR)/NotoSans-Bold.ttf \
		$(FONT_NOTO_SANS_DL_DIR)/LICENSE $(@D)/
endef

define FONT_NOTO_SANS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 -t $(TARGET_DIR)/usr/share/fonts/noto \
		$(@D)/NotoSans-Regular.ttf $(@D)/NotoSans-Bold.ttf
endef

$(eval $(generic-package))
