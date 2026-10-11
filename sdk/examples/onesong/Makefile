# One Song, built with the Chumby SDK: "chumby-app build" runs this with
# CC and PKG_CONFIG pointing into the SDK (cross-compiling for the Chumby).
# The bundle is everything in bundle/: app.conf and the program, plus any
# pictures or sounds the app reads from chumby_app_dir().
CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS += -O2 -Wall $(shell $(PKG_CONFIG) --cflags libchumby)
LDLIBS += $(shell $(PKG_CONFIG) --libs libchumby)

all: bundle/onesong bundle/app.conf

bundle/onesong: onesong.c
	mkdir -p bundle
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

bundle/app.conf: app.conf
	mkdir -p bundle
	cp $< $@

clean:
	rm -rf bundle *.chumbyapp

.PHONY: all clean
