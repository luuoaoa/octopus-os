# Built with the Chumby SDK: "chumby-app build" runs this with CC and
# PKG_CONFIG pointing into the SDK. The bundle is everything in bundle/.
CC ?= cc
PKG_CONFIG ?= pkg-config

override CFLAGS += -O2 -Wall $(shell $(PKG_CONFIG) --cflags libchumby)
override LDLIBS += $(shell $(PKG_CONFIG) --libs libchumby)

all: bundle/hello bundle/app.conf

bundle/hello: hello.c
	mkdir -p bundle
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

bundle/app.conf: app.conf
	mkdir -p bundle
	cp $< $@

clean:
	rm -rf bundle *.chumbyapp

.PHONY: all clean
