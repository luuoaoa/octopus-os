# octopus-apps: one binary per app, all on liboctopus.
PKG_CONFIG ?= pkg-config
# (Podcasts and Internet Radio are app bundles: apps/, package/octopus-bundled-apps.)
APPS := octopus-clock octopus-fm octopus-stream octopus-calibrate octopus-airplay \
	octopus-airplay-meta octopus-airplay-remote octopus-toasters octopus-settings

override CFLAGS += -Wall $(shell $(PKG_CONFIG) --cflags liboctopus)
LDLIBS += $(shell $(PKG_CONFIG) --libs liboctopus)

all: $(APPS)

octopus-clock: clock.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

octopus-fm: fm.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

octopus-airplay: airplay.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# shairport-sync's metadata pipe to a state file: no UI, no libraries.
octopus-airplay-meta: airplay-meta.o
	$(CC) $(LDFLAGS) -o $@ $^

# Remote control of the AirPlay sender (DACP): no UI, no libraries.
octopus-airplay-remote: airplay-remote.o
	$(CC) $(LDFLAGS) -o $@ $^

# A screen saver, on liboctopus's saver kit (pixman, no LVGL).
octopus-toasters: toasters.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(shell $(PKG_CONFIG) --libs pixman-1)

octopus-settings: settings.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

octopus-calibrate: calibrate.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lm

# The internet radio player: no UI, liboctopus only for its paths.
stream.o: override CFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl libmpg123 alsa)
octopus-stream: stream.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(shell $(PKG_CONFIG) --libs libcurl libmpg123 alsa) \
		-lfaad_fixed -lpthread -lm

clean:
	rm -f $(APPS) *.o

.PHONY: all clean
