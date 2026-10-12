# Podcasts: an app bundle, built like any (docs/sdk.md): with the SDK's
# octopus-app build, or by the image (package/octopus-bundled-apps), CC and
# PKG_CONFIG pointing into the toolchain. The bundle is bundle/.
CC ?= cc
PKG_CONFIG ?= pkg-config

override CFLAGS += -O2 -Wall

all: bundle/octopus-podcasts bundle/octopus-podcast-feed bundle/app.conf bundle/podcasts.conf

bundle/octopus-podcasts: podcasts.c
	mkdir -p bundle
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags liboctopus) -o $@ $< \
		$(LDFLAGS) $(shell $(PKG_CONFIG) --libs liboctopus)

# A feed, parsed as it downloads: no UI.
bundle/octopus-podcast-feed: podcast-feed.c
	mkdir -p bundle
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags libcurl expat) -o $@ $< \
		$(LDFLAGS) $(shell $(PKG_CONFIG) --libs libcurl expat)

bundle/%.conf: %.conf
	mkdir -p bundle
	cp $< $@

clean:
	rm -rf bundle *.octopusapp

.PHONY: all clean
