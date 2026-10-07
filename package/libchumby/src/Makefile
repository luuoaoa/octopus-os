# libchumby: LVGL (unpacked into ./lvgl by the package) + the framework,
# as one shared library. The package passes PKG_CONFIG, WAYLAND_SCANNER
# and WAYLAND_PROTOCOLS.
PKG_CONFIG ?= pkg-config
PKGS := freetype2 wayland-client wayland-cursor xkbcommon alsa
SONAME := libchumby.so.1

LVGL_PATH := lvgl
PROTO_SRCS := wayland_xdg_shell.c wayland_xdg_output.c
PROTO_HDRS := $(PROTO_SRCS:.c=.h)
CSRCS := $(shell find $(LVGL_PATH)/src -type f -name '*.c') chumby.c $(PROTO_SRCS)
OBJS := $(CSRCS:.c=.o)

override CFLAGS += -fPIC -Wall -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL_PATH) \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: $(SONAME)

wayland_xdg_shell.h:
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
wayland_xdg_shell.c:
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
wayland_xdg_output.h:
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS)/unstable/xdg-output/xdg-output-unstable-v1.xml $@
wayland_xdg_output.c:
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS)/unstable/xdg-output/xdg-output-unstable-v1.xml $@

$(OBJS): lv_conf.h $(PROTO_HDRS)

$(SONAME): $(OBJS)
	$(CC) -shared -Wl,-soname,$(SONAME) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(SONAME) $(OBJS) $(PROTO_SRCS) $(PROTO_HDRS)

.PHONY: all clean
