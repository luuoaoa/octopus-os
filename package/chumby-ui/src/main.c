// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-ui: LVGL home screen for the Chumby One (320x240).
 *
 *   clock + date, FM radio (station, tune, on/off), volume, IP address.
 *
 * Inputs: touchscreen (mxs-lradc-ts via LVGL evdev) and the knob
 * ("rotary", REL_WHEEL), which adjusts the volume. The radio itself is
 * driven through /usr/bin/radio, run as a child process so the UI never
 * blocks (tuner init takes ~2.5 s).
 */

#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

extern char **environ;

#define FB_DEV		"/dev/fb0"
#define STATION_FILE	"/etc/radio.station"
#define RADIO_PIDFILE	"/var/run/radio-loop.pid"
#define TOUCH_CONF	"/etc/chumby-ui/touch.conf"
#define FREQ_MIN	876	/* tenths of MHz */
#define FREQ_MAX	1080

/* ------------------------------------------------------------------ */
/* State                                                              */

static int freq = 1003;		/* tenths of MHz */
static bool radio_on;
static int volume = 70;		/* percent */

static lv_obj_t *lbl_time, *lbl_date, *lbl_freq, *lbl_radio, *btn_power;
static lv_obj_t *slider_vol, *lbl_vol, *lbl_status;
static lv_timer_t *tune_timer;

static int knob_fd = -1;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */

static uint32_t tick_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

/* Find /dev/input/eventN whose name matches `name`; returns an fd or -1. */
static int find_input(const char *name, char *path, size_t len, int flags)
{
	char dev[32], devname[64];
	int i, fd;

	for (i = 0; i < 16; i++) {
		snprintf(dev, sizeof(dev), "/dev/input/event%d", i);
		fd = open(dev, flags);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(devname)), devname) >= 0 &&
		    !strcmp(devname, name)) {
			if (path)
				snprintf(path, len, "%s", dev);
			return fd;
		}
		close(fd);
	}
	return -1;
}

static bool radio_running(void)
{
	FILE *f = fopen(RADIO_PIDFILE, "r");
	int pid = 0;

	if (!f)
		return false;
	if (fscanf(f, "%d", &pid) != 1)
		pid = 0;
	fclose(f);
	return pid > 0 && kill(pid, 0) == 0;
}

static void load_station(void)
{
	FILE *f = fopen(STATION_FILE, "r");
	double mhz;

	if (f && fscanf(f, "%lf", &mhz) == 1 && mhz >= 76 && mhz <= 108)
		freq = (int)(mhz * 10 + 0.5);
	if (f)
		fclose(f);
}

static void save_station(void)
{
	FILE *f = fopen(STATION_FILE, "w");

	if (f) {
		fprintf(f, "%d.%d\n", freq / 10, freq % 10);
		fclose(f);
	}
}

/* ------------------------------------------------------------------ */
/* Volume (ALSA "DAC" simple element, set in dB)                      */

static snd_mixer_t *mixer;
static snd_mixer_elem_t *dac, *hp;

static void mixer_open(void)
{
	snd_mixer_selem_id_t *sid;

	if (snd_mixer_open(&mixer, 0) < 0)
		return;
	if (snd_mixer_attach(mixer, "default") < 0 ||
	    snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(mixer) < 0) {
		static bool warned;

		snd_mixer_close(mixer);
		mixer = NULL;
		if (!warned)
			LV_LOG_WARN("no ALSA mixer yet; will retry");
		warned = true;
		return;
	}
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_name(sid, "DAC");
	dac = snd_mixer_find_selem(mixer, sid);
	snd_mixer_selem_id_set_name(sid, "HP");
	hp = snd_mixer_find_selem(mixer, sid);
}

static void volume_apply(void)
{
	/* 0 % = muted, 1..100 % = -50 dB .. 0 dB on the DAC */
	long db = volume ? -5000 + volume * 50 : -12700;

	if (dac) {
		snd_mixer_selem_set_playback_dB_all(dac, db, 0);
		if (snd_mixer_selem_has_playback_switch(dac))
			snd_mixer_selem_set_playback_switch_all(dac, volume > 0);
	}
	if (hp && snd_mixer_selem_has_playback_switch(hp))
		snd_mixer_selem_set_playback_switch_all(hp, volume > 0);

	lv_slider_set_value(slider_vol, volume, LV_ANIM_OFF);
	lv_label_set_text_fmt(lbl_vol, "%d%%", volume);
}

static void volume_change(int delta)
{
	volume += delta;
	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;
	volume_apply();
}

/* ------------------------------------------------------------------ */
/* Radio control: one child at a time, latest request wins            */

enum { REQ_NONE, REQ_ON, REQ_OFF, REQ_TUNE };
static pid_t child = -1;
static int pending = REQ_NONE;

static void update_radio_ui(const char *busy)
{
	lv_label_set_text_fmt(lbl_freq, "%d.%d", freq / 10, freq % 10);
	if (busy)
		lv_label_set_text(lbl_radio, busy);
	else
		lv_label_set_text(lbl_radio, radio_on ? "FM  " LV_SYMBOL_AUDIO "  on" : "FM  off");
	if (radio_on)
		lv_obj_add_state(btn_power, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(btn_power, LV_STATE_CHECKED);
}

static void run_request(int req)
{
	char f[16];
	char *argv[4] = { "radio", NULL, NULL, NULL };

	snprintf(f, sizeof(f), "%d.%d", freq / 10, freq % 10);
	switch (req) {
	case REQ_ON:
		argv[1] = "on";
		argv[2] = f;
		update_radio_ui("FM  starting...");
		break;
	case REQ_OFF:
		argv[1] = "off";
		update_radio_ui("FM  stopping...");
		break;
	case REQ_TUNE:
		argv[1] = "tune";
		argv[2] = f;
		update_radio_ui("FM  tuning...");
		break;
	default:
		return;
	}
	if (posix_spawn(&child, "/usr/bin/radio", NULL, NULL, argv, environ)) {
		child = -1;
		update_radio_ui("FM  error");
	}
}

static void radio_request(int req)
{
	if (child > 0) {
		pending = req;	/* run once the current command finishes */
		return;
	}
	run_request(req);
}

static void child_poll(lv_timer_t *t)
{
	int status;

	(void)t;
	if (child <= 0 || waitpid(child, &status, WNOHANG) != child)
		return;
	child = -1;
	radio_on = radio_running();
	update_radio_ui(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? NULL : "FM  error");
	if (pending != REQ_NONE) {
		int req = pending;

		pending = REQ_NONE;
		run_request(req);
	}
}

static void radio_toggle(void)
{
	radio_on = !radio_on;	/* optimistic; corrected when the child exits */
	radio_request(radio_on ? REQ_ON : REQ_OFF);
}

/* Debounced tune: wait until the user stops stepping. */
static void tune_timer_cb(lv_timer_t *t)
{
	(void)t;
	lv_timer_pause(tune_timer);
	save_station();
	if (radio_on)
		radio_request(REQ_TUNE);
}

static void freq_step(int delta)
{
	freq += delta;
	if (freq < FREQ_MIN)
		freq = FREQ_MAX;
	if (freq > FREQ_MAX)
		freq = FREQ_MIN;
	update_radio_ui(NULL);
	lv_timer_reset(tune_timer);
	lv_timer_resume(tune_timer);
}

/* ------------------------------------------------------------------ */
/* Knob                                                               */

static void knob_poll(lv_timer_t *t)
{
	struct input_event ev;
	int wheel = 0;

	(void)t;
	while (knob_fd >= 0 && read(knob_fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_REL && ev.code == REL_WHEEL)
			wheel += ev.value;
	if (wheel)
		volume_change(wheel * 2);
}

/* ------------------------------------------------------------------ */
/* Clock and status                                                   */

static void clock_cb(lv_timer_t *t)
{
	char buf[32];
	time_t now = time(NULL);
	struct tm tm;

	(void)t;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), "%H:%M", &tm);
	lv_label_set_text(lbl_time, buf);
	strftime(buf, sizeof(buf), "%A %d %B", &tm);
	lv_label_set_text(lbl_date, buf);
}

static void status_cb(lv_timer_t *t)
{
	struct ifaddrs *ifa, *i;
	char ip[INET_ADDRSTRLEN] = "";
	char host[64] = "";

	(void)t;

	/* The sound card is a module and may appear after we start. */
	if (!mixer) {
		mixer_open();
		if (mixer)
			volume_apply();
	}

	gethostname(host, sizeof(host) - 1);
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET ||
			    !strcmp(i->ifa_name, "lo"))
				continue;
			inet_ntop(AF_INET,
				  &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
				  ip, sizeof(ip));
			break;
		}
		freeifaddrs(ifa);
	}
	lv_label_set_text_fmt(lbl_status, LV_SYMBOL_WIFI "  %s   %s", host,
			      ip[0] ? ip : "no network");
}

/* ------------------------------------------------------------------ */
/* UI                                                                 */

static void btn_step_cb(lv_event_t *e)
{
	freq_step((int)(intptr_t)lv_event_get_user_data(e));
}

static void btn_power_cb(lv_event_t *e)
{
	(void)e;
	radio_toggle();
}

static void slider_cb(lv_event_t *e)
{
	volume = lv_slider_get_value(lv_event_get_target_obj(e));
	volume_apply();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *sym, int w)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, w, 48);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
	lv_obj_center(l);
	return b;
}

static void build_ui(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *card, *b, *l;

	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
	lv_obj_set_scrollable(scr, false);

	lbl_time = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(lbl_time, lv_color_white(), 0);
	lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 4);

	lbl_date = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_date, lv_color_hex(0x9aa4ae), 0);
	lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, 60);

	/* FM card: [<] 100.3 MHz [>] [power] */
	card = lv_obj_create(scr);
	lv_obj_set_size(card, 308, 92);
	lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 84);
	lv_obj_set_style_pad_all(card, 6, 0);
	lv_obj_set_scrollable(card, false);

	b = make_button(card, LV_SYMBOL_LEFT, 52);
	lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 6);
	lv_obj_add_event_cb(b, btn_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
	lv_obj_add_event_cb(b, btn_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);

	lbl_freq = lv_label_create(card);
	lv_obj_set_style_text_font(lbl_freq, &lv_font_montserrat_28, 0);
	lv_obj_align(lbl_freq, LV_ALIGN_CENTER, -34, 8);

	l = lv_label_create(card);
	lv_label_set_text(l, "MHz");
	lv_obj_align(l, LV_ALIGN_CENTER, 22, 12);

	lbl_radio = lv_label_create(card);
	lv_obj_align(lbl_radio, LV_ALIGN_TOP_MID, -26, -2);

	b = make_button(card, LV_SYMBOL_RIGHT, 52);
	lv_obj_align(b, LV_ALIGN_RIGHT_MID, -58, 6);
	lv_obj_add_event_cb(b, btn_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
	lv_obj_add_event_cb(b, btn_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);

	btn_power = make_button(card, LV_SYMBOL_POWER, 52);
	lv_obj_set_checkable(btn_power, true);
	lv_obj_align(btn_power, LV_ALIGN_RIGHT_MID, 0, 6);
	lv_obj_add_event_cb(btn_power, btn_power_cb, LV_EVENT_CLICKED, NULL);

	/* Volume (also on the knob) */
	l = lv_label_create(scr);
	lv_label_set_text(l, LV_SYMBOL_VOLUME_MAX);
	lv_obj_set_style_text_color(l, lv_color_white(), 0);
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 12, 190);

	slider_vol = lv_slider_create(scr);
	lv_obj_set_width(slider_vol, 220);
	lv_slider_set_range(slider_vol, 0, 100);
	lv_obj_align(slider_vol, LV_ALIGN_TOP_LEFT, 40, 194);
	lv_obj_add_event_cb(slider_vol, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lbl_vol = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_vol, lv_color_white(), 0);
	lv_obj_align(lbl_vol, LV_ALIGN_TOP_RIGHT, -8, 190);

	lbl_status = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x9aa4ae), 0);
	lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* ------------------------------------------------------------------ */

static int tty_fd = -1;

static void restore_console(void)
{
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
}

static void on_signal(int sig)
{
	restore_console();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void setup_touch(void)
{
	char path[32];
	int fd = find_input("mxs-lradc-ts", path, sizeof(path), O_RDONLY);
	/* Vendor (stmp3xxx_ts.c) calibration: X 3930..125, Y 130..4000 */
	int min_x = 3930, min_y = 130, max_x = 125, max_y = 4000, swap = 0;
	lv_indev_t *ts;
	FILE *f;

	if (fd < 0) {
		LV_LOG_WARN("no touchscreen found");
		return;
	}
	close(fd);

	f = fopen(TOUCH_CONF, "r");
	if (f) {
		if (fscanf(f, "%d %d %d %d %d", &min_x, &min_y, &max_x, &max_y,
			   &swap) < 4)
			LV_LOG_WARN("ignoring malformed " TOUCH_CONF);
		fclose(f);
	}

	ts = lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
	if (!ts)
		return;
	lv_evdev_set_swap_axes(ts, swap);
	lv_evdev_set_calibration(ts, min_x, min_y, max_x, max_y);
}

int main(void)
{
	lv_display_t *disp;
	int i;

	/* The DRM driver (a module) may still be loading when we start. */
	for (i = 0; i < 100 && access(FB_DEV, F_OK); i++)
		usleep(100000);

	/* Keep fbcon from drawing over us. */
	tty_fd = open("/dev/tty0", O_RDWR);
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
	atexit(restore_console);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	lv_init();
	lv_tick_set_cb(tick_ms);

	disp = lv_linux_fbdev_create();
	if (!disp) {
		fprintf(stderr, "chumby-ui: cannot create display\n");
		return 1;
	}
	lv_linux_fbdev_set_file(disp, FB_DEV);

	setup_touch();
	knob_fd = find_input("rotary", NULL, 0, O_RDONLY | O_NONBLOCK);
	if (knob_fd < 0)
		LV_LOG_WARN("no knob found");

	build_ui();
	mixer_open();
	load_station();
	radio_on = radio_running();
	volume_apply();
	update_radio_ui(NULL);
	clock_cb(NULL);
	status_cb(NULL);

	lv_timer_create(clock_cb, 1000, NULL);
	lv_timer_create(status_cb, 5000, NULL);
	lv_timer_create(knob_poll, 20, NULL);
	lv_timer_create(child_poll, 100, NULL);
	tune_timer = lv_timer_create(tune_timer_cb, 400, NULL);
	lv_timer_pause(tune_timer);

	for (;;) {
		uint32_t idle = lv_timer_handler();

		if (idle > 50)
			idle = 50;
		usleep(idle * 1000);
	}
}
