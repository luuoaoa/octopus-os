# chumby-ui: LVGL is unpacked into ./lvgl by the buildroot package.
LVGL_PATH := lvgl
CSRCS := $(shell find $(LVGL_PATH)/src -type f -name '*.c') main.c
OBJS := $(CSRCS:.c=.o)

CFLAGS += -O2 -Wall -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL_PATH)
LDLIBS += -lasound -lm

all: chumby-ui

chumby-ui: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJS): lv_conf.h

clean:
	rm -f chumby-ui $(OBJS)

.PHONY: all clean
