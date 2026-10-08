# chumby-shell, chumby-alarm-tone and chumby-screenshot. The package passes
# PKG_CONFIG and WAYLAND_SCANNER.
PKG_CONFIG ?= pkg-config

FTL := wlr-foreign-toplevel-management-unstable-v1
SCP := wlr-screencopy-unstable-v1

override CFLAGS += -Wall -I.
SHELL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libchumby wayland-client)
SHELL_LIBS := $(shell $(PKG_CONFIG) --libs libchumby wayland-client)
WL_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client)
WL_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client)
ALSA_CFLAGS := $(shell $(PKG_CONFIG) --cflags alsa)
ALSA_LIBS := $(shell $(PKG_CONFIG) --libs alsa)

all: chumby-shell chumby-alarm-tone chumby-screenshot

%-client-protocol.h: protocol/%.xml
	$(WAYLAND_SCANNER) client-header $< $@
%-protocol.c: protocol/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

shell.o: shell.c $(FTL)-client-protocol.h
	$(CC) $(CFLAGS) $(SHELL_CFLAGS) -c -o $@ $<
screenshot.o: screenshot.c $(SCP)-client-protocol.h
	$(CC) $(CFLAGS) $(WL_CFLAGS) -c -o $@ $<
alarm-tone.o: alarm-tone.c
	$(CC) $(CFLAGS) $(ALSA_CFLAGS) -c -o $@ $<
%-protocol.o: %-protocol.c
	$(CC) $(CFLAGS) $(WL_CFLAGS) -c -o $@ $<

chumby-shell: shell.o $(FTL)-protocol.o
	$(CC) $(LDFLAGS) -o $@ $^ $(SHELL_LIBS)
chumby-alarm-tone: alarm-tone.o
	$(CC) $(LDFLAGS) -o $@ $^ $(ALSA_LIBS) -lm
chumby-screenshot: screenshot.o $(SCP)-protocol.o
	$(CC) $(LDFLAGS) -o $@ $^ $(WL_LIBS)

clean:
	rm -f chumby-shell chumby-alarm-tone chumby-screenshot *.o *-protocol.h *-protocol.c

.PHONY: all clean
.SECONDARY:
