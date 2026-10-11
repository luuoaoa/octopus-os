# Internet Radio: an app bundle, built like any (docs/sdk.md): with the
# SDK's chumby-app build, or by the image (package/chumby-bundled-apps).
# The bundle is bundle/.
CC ?= cc
PKG_CONFIG ?= pkg-config

override CFLAGS += -O2 -Wall $(shell $(PKG_CONFIG) --cflags libchumby)
override LDLIBS += $(shell $(PKG_CONFIG) --libs libchumby)

all: bundle/chumby-netradio bundle/app.conf bundle/streams.conf

bundle/chumby-netradio: netradio.c
	mkdir -p bundle
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

bundle/%.conf: %.conf
	mkdir -p bundle
	cp $< $@

clean:
	rm -rf bundle *.chumbyapp

.PHONY: all clean
