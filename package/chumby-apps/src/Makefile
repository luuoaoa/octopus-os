# chumby-apps: one binary per app, all on libchumby.
PKG_CONFIG ?= pkg-config
APPS := chumby-clock chumby-fm chumby-netradio chumby-stream chumby-calibrate \
	chumby-podcasts chumby-podcast-feed

override CFLAGS += -Wall $(shell $(PKG_CONFIG) --cflags libchumby)
LDLIBS += $(shell $(PKG_CONFIG) --libs libchumby)

all: $(APPS)

chumby-clock: clock.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

chumby-fm: fm.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

chumby-netradio: netradio.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

chumby-podcasts: podcasts.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# A podcast feed, parsed as it downloads: no UI.
podcast-feed.o: override CFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl expat)
chumby-podcast-feed: podcast-feed.o
	$(CC) $(LDFLAGS) -o $@ $^ $(shell $(PKG_CONFIG) --libs libcurl expat)

chumby-calibrate: calibrate.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lm

# The internet radio player: no UI, libchumby only for its paths.
stream.o: override CFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl libmpg123 alsa)
chumby-stream: stream.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(shell $(PKG_CONFIG) --libs libcurl libmpg123 alsa) \
		-lfaad_fixed -lpthread -lm

clean:
	rm -f $(APPS) *.o

.PHONY: all clean
