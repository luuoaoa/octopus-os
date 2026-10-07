# chumby-ui: LVGL is unpacked into ./lvgl by the buildroot package.
LVGL_PATH := lvgl
CSRCS := $(shell find $(LVGL_PATH)/src -type f -name '*.c') main.c
OBJS := $(CSRCS:.c=.o)

# FT_CFLAGS / FT_LIBS: from pkg-config freetype2 (passed by the package).
# "override": buildroot passes CFLAGS/LDFLAGS on the command line, which
# would otherwise discard these additions.
override CFLAGS += -O2 -Wall -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL_PATH) $(FT_CFLAGS)
override LDLIBS += -lasound -lm $(FT_LIBS)

all: chumby-ui

chumby-ui: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJS): lv_conf.h

clean:
	rm -f chumby-ui $(OBJS)

.PHONY: all clean
