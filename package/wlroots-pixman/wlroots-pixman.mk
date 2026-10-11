################################################################################
#
# wlroots-pixman
#
################################################################################

# buildroot's wlroots package requires EGL, GLES and GBM, which a GPU-less
# board like the i.MX23 doesn't have. wlroots itself only needs them for
# the gles2 renderer and the GBM allocator: build it with neither, so it
# uses its (always built) pixman software renderer and DRM dumb buffers.
# A board with a GPU (the Pi) can add the gles2 renderer and GBM allocator
# (BR2_PACKAGE_WLROOTS_PIXMAN_GLES2): wlroots then renders on the GPU, and
# chumby-wm falls back to pixman if EGL won't start (wlroots doesn't,
# where there's a render node).
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
	-Dsession=enabled \
	-Dcolor-management=disabled \
	-Dlibliftoff=disabled

ifeq ($(BR2_PACKAGE_WLROOTS_PIXMAN_GLES2),y)
WLROOTS_PIXMAN_DEPENDENCIES += libegl libgles libgbm
WLROOTS_PIXMAN_CONF_OPTS += -Drenderers=gles2 -Dallocators=gbm
else
WLROOTS_PIXMAN_CONF_OPTS += -Drenderers= -Dallocators=
endif

$(eval $(meson-package))
