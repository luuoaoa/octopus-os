# Podcasts: an app bundle, built like any (docs/sdk.md): with the SDK's
# chumby-app build, or by the image (package/chumby-bundled-apps), CC and
# PKG_CONFIG pointing into the toolchain. The bundle is bundle/.
CC ?= cc
PKG_CONFIG ?= pkg-config

override CFLAGS += -O2 -Wall

all: bundle/chumby-podcasts bundle/chumby-podcast-feed bundle/app.conf bundle/podcasts.conf

bundle/chumby-podcasts: podcasts.c
	mkdir -p bundle
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags libchumby) -o $@ $< \
		$(LDFLAGS) $(shell $(PKG_CONFIG) --libs libchumby)

# A feed, parsed as it downloads: no UI.
bundle/chumby-podcast-feed: podcast-feed.c
	mkdir -p bundle
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags libcurl expat) -o $@ $< \
		$(LDFLAGS) $(shell $(PKG_CONFIG) --libs libcurl expat)

bundle/%.conf: %.conf
	mkdir -p bundle
	cp $< $@

clean:
	rm -rf bundle *.chumbyapp

.PHONY: all clean
