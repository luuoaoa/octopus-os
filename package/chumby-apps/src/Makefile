# chumby-apps: one binary per app, all on libchumby.
PKG_CONFIG ?= pkg-config
APPS := chumby-clock chumby-fm

override CFLAGS += -Wall $(shell $(PKG_CONFIG) --cflags libchumby)
LDLIBS += $(shell $(PKG_CONFIG) --libs libchumby)

all: $(APPS)

chumby-clock: clock.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

chumby-fm: fm.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(APPS) *.o

.PHONY: all clean
