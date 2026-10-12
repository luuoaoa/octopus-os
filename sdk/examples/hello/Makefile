# Built with the Octopus SDK: "octopus-app build" runs this with CC and
# PKG_CONFIG pointing into the SDK. The bundle is everything in bundle/.
CC ?= cc
PKG_CONFIG ?= pkg-config

override CFLAGS += -O2 -Wall $(shell $(PKG_CONFIG) --cflags liboctopus)
override LDLIBS += $(shell $(PKG_CONFIG) --libs liboctopus)

all: bundle/hello bundle/app.conf

bundle/hello: hello.c
	mkdir -p bundle
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

bundle/app.conf: app.conf
	mkdir -p bundle
	cp $< $@

clean:
	rm -rf bundle *.octopusapp

.PHONY: all clean
