# chumby-wm. The package passes PKG_CONFIG (target), WAYLAND_SCANNER (host)
# and WAYLAND_PROTOCOLS (staging pkgdatadir).
PKG_CONFIG ?= pkg-config
PKGS := wlroots-0.19 wayland-server xkbcommon pixman-1 libinput libdrm

override CFLAGS += -Wall -DWLR_USE_UNSTABLE -I. $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: chumby-wm

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

chumby-wm.o: chumby-wm.c xdg-shell-protocol.h

chumby-wm: chumby-wm.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f chumby-wm chumby-wm.o xdg-shell-protocol.h

.PHONY: all clean
