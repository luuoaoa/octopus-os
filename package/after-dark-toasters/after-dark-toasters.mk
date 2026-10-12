################################################################################
#
# after-dark-toasters
#
################################################################################

# The Flying Toasters' GIFs from After Dark CSS (its code is MIT; the
# pictures are Berkeley Systems'): fetched for the image, never committed.
AFTER_DARK_TOASTERS_VERSION = 1
AFTER_DARK_TOASTERS_SITE = https://www.bryanbraun.com/after-dark-css/img
AFTER_DARK_TOASTERS_SOURCE = toaster-sprite.gif
AFTER_DARK_TOASTERS_EXTRA_DOWNLOADS = toast1.gif
AFTER_DARK_TOASTERS_LICENSE = Copyright Berkeley Systems

# Pictures, not an archive: copied as they are.
define AFTER_DARK_TOASTERS_EXTRACT_CMDS
	cp $(AFTER_DARK_TOASTERS_DL_DIR)/toaster-sprite.gif \
		$(AFTER_DARK_TOASTERS_DL_DIR)/toast1.gif $(@D)/
endef

define AFTER_DARK_TOASTERS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/toaster-sprite.gif \
		$(TARGET_DIR)/usr/share/octopus/toasters/toaster-sprite.gif
	$(INSTALL) -D -m 0644 $(@D)/toast1.gif \
		$(TARGET_DIR)/usr/share/octopus/toasters/toast1.gif
endef

$(eval $(generic-package))
