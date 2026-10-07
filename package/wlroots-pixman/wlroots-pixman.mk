################################################################################
#
# wlroots-pixman
#
################################################################################

# buildroot's wlroots package requires EGL, GLES and GBM, which a GPU-less
# board like the i.MX23 doesn't have. wlroots itself only needs them for
# the gles2 renderer and the GBM allocator: build it with neither, so it
# uses its (always built) pixman software renderer and DRM dumb buffers.
WLROOTS_PIXMAN_VERSION = 0.19.2
WLROOTS_PIXMAN_SOURCE = wlroots-$(WLROOTS_PIXMAN_VERSION).tar.gz
WLROOTS_PIXMAN_SITE = https://gitlab.freedesktop.org/wlroots/wlroots/-/releases/$(WLROOTS_PIXMAN_VERSION)/downloads
WLROOTS_PIXMAN_LICENSE = MIT
WLROOTS_PIXMAN_LICENSE_FILES = LICENSE
WLROOTS_PIXMAN_INSTALL_STAGING = YES

WLROOTS_PIXMAN_DEPENDENCIES = \
	host-pkgconf \
	host-wayland \
	hwdata \
	libdisplay-info \
	libdrm \
	libinput \
	libxkbcommon \
	pixman \
	seatd \
	udev \
	wayland \
	wayland-protocols

WLROOTS_PIXMAN_CONF_OPTS = \
	-Dexamples=false \
	-Dxcb-errors=disabled \
	-Dxwayland=disabled \
	-Dbackends=drm,libinput \
	-Drenderers= \
	-Dallocators= \
	-Dsession=enabled \
	-Dcolor-management=disabled \
	-Dlibliftoff=disabled

$(eval $(meson-package))
