# One Song, built with the Octopus SDK: "octopus-app build" runs this with
# CC and PKG_CONFIG pointing into the SDK (cross-compiling for the device).
# The bundle is everything in bundle/: app.conf and the program, plus any
# pictures or sounds the app reads from octopus_app_dir().
CC ?= cc
PKG_CONFIG ?= pkg-config

override CFLAGS += -O2 -Wall $(shell $(PKG_CONFIG) --cflags liboctopus)
override LDLIBS += $(shell $(PKG_CONFIG) --libs liboctopus)

all: bundle/onesong bundle/app.conf

bundle/onesong: onesong.c
	mkdir -p bundle
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

bundle/app.conf: app.conf
	mkdir -p bundle
	cp $< $@

clean:
	rm -rf bundle *.octopusapp

.PHONY: all clean
