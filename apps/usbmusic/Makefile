# USB Music: an app bundle, built like any (docs/sdk.md): with the SDK's
# octopus-app build, or by the image (package/octopus-bundled-apps). The
# bundle is bundle/.
CC ?= cc
PKG_CONFIG ?= pkg-config

override CFLAGS += -O2 -Wall $(shell $(PKG_CONFIG) --cflags liboctopus)
override LDLIBS += $(shell $(PKG_CONFIG) --libs liboctopus)

all: bundle/octopus-usbmusic bundle/app.conf

bundle/octopus-usbmusic: usbmusic.c
	mkdir -p bundle
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

bundle/app.conf: app.conf
	mkdir -p bundle
	cp $< $@

clean:
	rm -rf bundle *.octopusapp

.PHONY: all clean
