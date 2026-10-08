// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * libchumby: common framework for Chumby One apps. See chumby.h.
 */

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "chumby.h"

extern char **environ;

#define POLICY_FILE	"/etc/chumby/policy.conf"
#define ALARMS_FILE	"/etc/chumby/alarms.conf"
#define RUN_DIR		"/run/chumby"
#define FONT_DIR	"/usr/share/fonts/noto/"
#define BACKLIGHT	"/sys/class/backlight/backlight/"
#define MAX_WINDOWS	4
#define MAX_FDS		8
#define MAX_MODE_CBS	8

static bool inited;
static lv_display_t *windows[MAX_WINDOWS];
static int n_windows;

/* ------------------------------------------------------------------ */
/* Policy                                                             */

static struct chumby_policy policy = {
	.night_start = 19 * 60,
	.night_end = 7 * 60,
	.force = -1,
	.day_brightness = 80,
	.night_brightness = 20,
	.screen_off_start = 21 * 60,
	.screen_off_end = 7 * 60,
	.wake_seconds = 10,
	.snooze_minutes = 9,
	.alarm_volume = 60,
};

static int parse_hhmm(const char *s, int def)
{
	int h, m;

	if (sscanf(s, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60)
		return h * 60 + m;
	return def;
}

static void policy_load(void)
{
	FILE *f = fopen(POLICY_FILE, "r");
	char line[128], key[64], val[64];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || sscanf(line, " %63[^= ] = %63s", key, val) != 2)
			continue;
		if (!strcmp(key, "night_start"))
			policy.night_start = parse_hhmm(val, policy.night_start);
		else if (!strcmp(key, "night_end"))
			policy.night_end = parse_hhmm(val, policy.night_end);
		else if (!strcmp(key, "mode"))
			policy.force = !strcmp(val, "day") ? CHUMBY_MODE_DAY :
				       !strcmp(val, "night") ? CHUMBY_MODE_NIGHT : -1;
		else if (!strcmp(key, "day_brightness"))
			policy.day_brightness = atoi(val);
		else if (!strcmp(key, "night_brightness"))
			policy.night_brightness = atoi(val);
		else if (!strcmp(key, "screen_off_start"))
			policy.screen_off_start = parse_hhmm(val, policy.screen_off_start);
		else if (!strcmp(key, "screen_off_end"))
			policy.screen_off_end = parse_hhmm(val, policy.screen_off_end);
		else if (!strcmp(key, "wake_seconds") && atoi(val) > 0)
			policy.wake_seconds = atoi(val);
		else if (!strcmp(key, "snooze_minutes") && atoi(val) > 0)
			policy.snooze_minutes = atoi(val);
		else if (!strcmp(key, "alarm_volume"))
			policy.alarm_volume = atoi(val);
	}
	fclose(f);
}

const struct chumby_policy *chumby_policy(void)
{
	return &policy;
}

bool chumby_in_window(int start, int end)
{
	time_t t = time(NULL);
	struct tm tm;
	int now;

	localtime_r(&t, &tm);
	now = tm.tm_hour * 60 + tm.tm_min;
	/* Night windows usually wrap midnight (19:00 -> 07:00). */
	if (start > end)
		return now >= start || now < end;
	return now >= start && now < end;
}

static chumby_mode_t policy_mode_now(void)
{
	if (policy.force >= 0)
		return policy.force;
	return chumby_in_window(policy.night_start, policy.night_end) ?
		CHUMBY_MODE_NIGHT : CHUMBY_MODE_DAY;
}

/* ------------------------------------------------------------------ */
/* Theme                                                              */

static chumby_mode_t mode = CHUMBY_MODE_DAY;

static const uint32_t palette[2][CHUMBY_COLOR_COUNT] = {
	[CHUMBY_MODE_DAY] = {
		[CHUMBY_COLOR_BG]	= 0x101418,
		[CHUMBY_COLOR_SURFACE]	= 0x1e252c,
		[CHUMBY_COLOR_TEXT]	= 0xe8edf2,
		[CHUMBY_COLOR_TEXT_DIM]	= 0x8c97a3,
		[CHUMBY_COLOR_ACCENT]	= 0x3d9bff,
	},
	/* Night: near-black, warm and dim so it doesn't light up a bedroom. */
	[CHUMBY_MODE_NIGHT] = {
		[CHUMBY_COLOR_BG]	= 0x000000,
		[CHUMBY_COLOR_SURFACE]	= 0x140c0a,
		[CHUMBY_COLOR_TEXT]	= 0xb8705a,
		[CHUMBY_COLOR_TEXT_DIM]	= 0x6a4034,
		[CHUMBY_COLOR_ACCENT]	= 0xa8422a,
	},
};

lv_color_t chumby_color(chumby_color_t c)
{
	return lv_color_hex(palette[mode][c < CHUMBY_COLOR_COUNT ? c : 0]);
}

chumby_mode_t chumby_mode(void)
{
	return mode;
}

static const lv_font_t *fonts[CHUMBY_FONT_COUNT];

static const lv_font_t *noto(const char *file, uint32_t size,
			     const lv_font_t *fallback)
{
	lv_font_t *f = lv_freetype_font_create(file,
					       LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
					       size, LV_FREETYPE_FONT_STYLE_NORMAL);

	/* Noto has no LV_SYMBOL_* glyphs: fall back to a bitmap font for those. */
	if (!f) {
		LV_LOG_WARN("cannot load %s at %u px, using built-in font",
			    file, (unsigned int)size);
		return fallback;
	}
	f->fallback = fallback;
	return f;
}

static void fonts_load(void)
{
	fonts[CHUMBY_FONT_SMALL] = noto(FONT_DIR "NotoSans-Regular.ttf", 13, &lv_font_montserrat_14);
	fonts[CHUMBY_FONT_BODY] = noto(FONT_DIR "NotoSans-Regular.ttf", 15, &lv_font_montserrat_14);
	fonts[CHUMBY_FONT_LARGE] = noto(FONT_DIR "NotoSans-Bold.ttf", 24, &lv_font_montserrat_20);
	fonts[CHUMBY_FONT_XL] = noto(FONT_DIR "NotoSans-Bold.ttf", 34, &lv_font_montserrat_20);
	fonts[CHUMBY_FONT_HUGE] = noto(FONT_DIR "NotoSans-Bold.ttf", 56, &lv_font_montserrat_20);
}

const lv_font_t *chumby_font(chumby_font_t f)
{
	return fonts[f < CHUMBY_FONT_COUNT ? f : CHUMBY_FONT_BODY];
}

static struct {
	chumby_mode_cb_t cb;
	void *user;
} mode_cbs[MAX_MODE_CBS];
static int n_mode_cbs;

/*
 * The chumby theme: LVGL's default (dark) theme as parent, plus palette
 * styles for the common widgets so every app's buttons and sliders follow
 * day/night without per-app code. The default theme draws button labels in
 * a fixed white, which would glare at night.
 */
static lv_theme_t *chumby_theme;
static lv_style_t st_btn, st_btn_checked, st_track;

static void theme_styles_init(void)
{
	static bool once;

	if (!once) {
		lv_style_init(&st_btn);
		lv_style_init(&st_btn_checked);
		lv_style_init(&st_track);
		once = true;
	}
	lv_style_set_bg_color(&st_btn, chumby_color(CHUMBY_COLOR_SURFACE));
	lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
	lv_style_set_text_color(&st_btn, chumby_color(CHUMBY_COLOR_TEXT));
	lv_style_set_shadow_width(&st_btn, 0);
	lv_style_set_bg_color(&st_btn_checked, chumby_color(CHUMBY_COLOR_ACCENT));
	/* Tracks must stay visible on the night palette's black. */
	lv_style_set_bg_color(&st_track, chumby_color(CHUMBY_COLOR_TEXT_DIM));
	lv_style_set_bg_opa(&st_track, LV_OPA_40);
}

static void chumby_theme_apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
	(void)th;
	if (lv_obj_check_type(obj, &lv_button_class)) {
		lv_obj_add_style(obj, &st_btn, 0);
		lv_obj_add_style(obj, &st_btn_checked, LV_STATE_CHECKED);
	} else if (lv_obj_check_type(obj, &lv_slider_class)) {
		lv_obj_add_style(obj, &st_track, LV_PART_MAIN);
	}
}

static void theme_apply(lv_display_t *disp)
{
	lv_theme_t *th = lv_theme_default_init(disp, chumby_color(CHUMBY_COLOR_ACCENT),
					       chumby_color(CHUMBY_COLOR_ACCENT),
					       true, chumby_font(CHUMBY_FONT_BODY));
	lv_obj_t *scr = lv_display_get_screen_active(disp);

	theme_styles_init();
	if (!chumby_theme) {
		chumby_theme = lv_theme_create();
		lv_theme_set_apply_cb(chumby_theme, chumby_theme_apply_cb);
	}
	lv_theme_set_parent(chumby_theme, th);
	lv_display_set_theme(disp, chumby_theme);
	lv_obj_set_style_bg_color(scr, chumby_color(CHUMBY_COLOR_BG), 0);
	lv_obj_set_style_text_color(scr, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_font(scr, chumby_font(CHUMBY_FONT_BODY), 0);
}

static void mode_set(chumby_mode_t m, bool force)
{
	int i;

	if (m == mode && !force)
		return;
	mode = m;
	for (i = 0; i < n_windows; i++)
		theme_apply(windows[i]);
	for (i = 0; i < n_mode_cbs; i++)
		mode_cbs[i].cb(mode, mode_cbs[i].user);
	lv_obj_report_style_change(NULL);
}

void chumby_on_mode_change(chumby_mode_cb_t cb, void *user)
{
	if (n_mode_cbs >= MAX_MODE_CBS)
		return;
	mode_cbs[n_mode_cbs].cb = cb;
	mode_cbs[n_mode_cbs].user = user;
	n_mode_cbs++;
	cb(mode, user);
}

static void policy_timer_cb(lv_timer_t *t)
{
	(void)t;
	policy_load();
	mode_set(policy_mode_now(), false);
}

/* ------------------------------------------------------------------ */
/* Init, windows, main loop                                           */

static uint32_t tick_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

void chumby_init(void)
{
	if (inited)
		return;
	inited = true;
	policy_load();
	mode = policy_mode_now();
	lv_init();
	lv_tick_set_cb(tick_ms);
	fonts_load();
	lv_timer_create(policy_timer_cb, 30000, NULL);
}

lv_display_t *chumby_window_create(const char *app_id, int width, int height)
{
	lv_display_t *disp;

	chumby_init();
	if (n_windows >= MAX_WINDOWS)
		return NULL;
	disp = lv_wayland_window_create(width, height, (char *)app_id, NULL);
	if (!disp)
		return NULL;
	windows[n_windows++] = disp;
	theme_apply(disp);
	return disp;
}

void chumby_window_close(lv_display_t *disp)
{
	int i;

	for (i = 0; i < n_windows; i++) {
		if (windows[i] != disp)
			continue;
		windows[i] = windows[--n_windows];
		lv_wayland_window_close(disp);
		return;
	}
}

lv_obj_t *chumby_app_init(const char *app_id)
{
	/* chumby-wm resizes app windows to the area right of the panel. */
	lv_display_t *disp = chumby_window_create(app_id, CHUMBY_APP_W, CHUMBY_SCREEN_H);

	if (!disp) {
		fprintf(stderr, "%s: cannot open a Wayland window "
			"(is chumby-wm running? WAYLAND_DISPLAY=%s)\n",
			app_id, getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "");
		exit(1);
	}
	return lv_display_get_screen_active(disp);
}

static struct {
	int fd;
	chumby_fd_cb_t cb;
	void *user;
} watches[MAX_FDS];
static int n_watches;

void chumby_watch_fd(int fd, chumby_fd_cb_t cb, void *user)
{
	if (fd < 0 || n_watches >= MAX_FDS)
		return;
	watches[n_watches].fd = fd;
	watches[n_watches].cb = cb;
	watches[n_watches].user = user;
	n_watches++;
}

int chumby_run(void)
{
	struct pollfd pfd[MAX_FDS + 1];
	int i, n;

	for (;;) {
		uint32_t idle = lv_timer_handler();

		if (idle > LV_DEF_REFR_PERIOD)
			idle = LV_DEF_REFR_PERIOD;

		/* The Wayland fd only wakes us early; LVGL reads it from its
		 * own timer inside lv_timer_handler(). */
		n = 0;
		pfd[n].fd = lv_wayland_get_fd();
		pfd[n++].events = POLLIN;
		for (i = 0; i < n_watches; i++) {
			pfd[n].fd = watches[i].fd;
			pfd[n++].events = POLLIN;
		}
		if (poll(pfd, n, idle) <= 0)
			continue;
		for (i = 0; i < n_watches; i++)
			if (pfd[i + 1].revents & (POLLIN | POLLHUP | POLLERR))
				watches[i].cb(watches[i].fd, watches[i].user);
	}
	return 0;
}

int chumby_spawn(const char *path, char *const argv[])
{
	pid_t pid;

	if (posix_spawn(&pid, path, NULL, NULL, argv, environ))
		return -1;
	return pid;
}

/* ------------------------------------------------------------------ */
/* Hardware helpers                                                   */

static snd_mixer_t *mixer;

static snd_mixer_elem_t *dac_elem(void)
{
	static snd_mixer_elem_t *dac;
	snd_mixer_selem_id_t *sid;

	if (dac) {
		/* Pick up changes made by other processes (the shell). */
		snd_mixer_handle_events(mixer);
		return dac;
	}
	if (!mixer) {
		if (snd_mixer_open(&mixer, 0) < 0)
			return NULL;
		if (snd_mixer_attach(mixer, "default") < 0 ||
		    snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
		    snd_mixer_load(mixer) < 0) {
			snd_mixer_close(mixer);
			mixer = NULL;
			return NULL;
		}
	}
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_name(sid, "DAC");
	dac = snd_mixer_find_selem(mixer, sid);
	return dac;
}

/* 0 % = muted, 1..100 % = -50 dB .. 0 dB on the codec's DAC. */
int chumby_volume_get(void)
{
	snd_mixer_elem_t *e = dac_elem();
	long db;
	int sw = 1;

	if (!e || snd_mixer_selem_get_playback_dB(e, SND_MIXER_SCHN_FRONT_LEFT, &db) < 0)
		return -1;
	if (snd_mixer_selem_has_playback_switch(e))
		snd_mixer_selem_get_playback_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &sw);
	if (!sw || db <= -5000)
		return 0;
	return (int)((db + 5000) / 50);
}

void chumby_volume_set(int percent)
{
	snd_mixer_elem_t *e = dac_elem();

	if (!e)
		return;
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	snd_mixer_selem_set_playback_dB_all(e, percent ? -5000 + percent * 50 : -12700, 0);
	if (snd_mixer_selem_has_playback_switch(e))
		snd_mixer_selem_set_playback_switch_all(e, percent > 0);
}

static int read_int(const char *path)
{
	FILE *f = fopen(path, "r");
	int v = -1;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

int chumby_brightness_get(void)
{
	int max = read_int(BACKLIGHT "max_brightness");
	int cur = read_int(BACKLIGHT "brightness");

	if (max <= 0 || cur < 0)
		return -1;
	return (cur * 100 + max / 2) / max;
}

void chumby_brightness_set(int percent)
{
	int max = read_int(BACKLIGHT "max_brightness");
	FILE *f;

	if (max <= 0)
		return;
	if (percent < 1)
		percent = 1;	/* never fully dark: the screen would look dead */
	if (percent > 100)
		percent = 100;
	f = fopen(BACKLIGHT "brightness", "w");
	if (f) {
		fprintf(f, "%d\n", (percent * max + 50) / 100 ? (percent * max + 50) / 100 : 1);
		fclose(f);
	}
}

/* FB_BLANK_UNBLANK / FB_BLANK_POWERDOWN: pwm-backlight turns the PWM off. */
void chumby_screen_power(bool on)
{
	FILE *f = fopen(BACKLIGHT "bl_power", "w");

	if (f) {
		fprintf(f, "%d\n", on ? 0 : 4);
		fclose(f);
	}
}

bool chumby_screen_is_on(void)
{
	return read_int(BACKLIGHT "bl_power") <= 0;
}

/* ------------------------------------------------------------------ */
/* Alarms                                                             */

static const char *const day_names[7] = {
	"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};

const char *chumby_alarms_path(void)
{
	const char *p = getenv("CHUMBY_ALARMS");

	return p && *p ? p : ALARMS_FILE;
}

int chumby_alarms_load(struct chumby_alarm *alarms, int max)
{
	FILE *f = fopen(chumby_alarms_path(), "r");
	char line[128], hhmm[16], days[16], state[16];
	int n = 0, i;

	if (!f)
		return 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		struct chumby_alarm *a = &alarms[n];

		if (line[0] == '#' ||
		    sscanf(line, "%15s %15s %15s", hhmm, days, state) != 3)
			continue;
		a->time = parse_hhmm(hhmm, -1);
		if (a->time < 0)
			continue;
		a->days = 0;
		if (strcmp(days, "once"))
			for (i = 0; i < 7 && days[i]; i++)
				if (days[i] != '-')
					a->days |= 1 << i;
		a->enabled = !strcmp(state, "on");
		n++;
	}
	fclose(f);
	return n;
}

/* Written to a temporary file and renamed: the shell may read it anytime. */
bool chumby_alarms_save(const struct chumby_alarm *alarms, int n)
{
	const char *path = chumby_alarms_path();
	char tmp[256];
	FILE *f;
	int i, d;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return false;
	fprintf(f, "# Chumby alarms: HH:MM, days Monday..Sunday ('-' = off) "
		   "or once, on/off.\n# Edited in the Clock app.\n");
	for (i = 0; i < n; i++) {
		fprintf(f, "%02d:%02d ", alarms[i].time / 60, alarms[i].time % 60);
		if (!alarms[i].days)
			fputs("once", f);
		for (d = 0; alarms[i].days && d < 7; d++)
			fputc(alarms[i].days & (1 << d) ? "MTWTFSS"[d] : '-', f);
		fprintf(f, " %s\n", alarms[i].enabled ? "on" : "off");
	}
	if (fflush(f) || fsync(fileno(f))) {
		fclose(f);
		unlink(tmp);
		return false;
	}
	fclose(f);
	return rename(tmp, path) == 0;
}

const char *chumby_alarm_days_text(unsigned char days, char *buf, size_t len)
{
	size_t used = 0;
	int d;

	if (days == CHUMBY_DAYS_ALL)
		return "Every day";
	if (days == CHUMBY_DAYS_WEEKDAYS)
		return "Weekdays";
	if (days == CHUMBY_DAYS_WEEKEND)
		return "Weekends";
	if (!days)
		return "Once";
	buf[0] = '\0';
	for (d = 0; d < 7; d++)
		if (days & (1 << d) && used < len)
			used += snprintf(buf + used, len - used, "%s%s",
					 used ? " " : "", day_names[d]);
	return buf;
}

bool chumby_alarm_on_day(const struct chumby_alarm *a, const struct tm *tm)
{
	/* tm_wday: 0 = Sunday; our bits start at Monday. */
	return !a->days || (a->days & (1 << ((tm->tm_wday + 6) % 7)));
}

time_t chumby_alarm_next(const struct chumby_alarm *a, time_t from)
{
	struct tm tm;
	int d;

	if (!a->enabled)
		return 0;
	for (d = 0; d < 8; d++) {
		time_t t;

		localtime_r(&from, &tm);
		tm.tm_mday += d;
		tm.tm_hour = a->time / 60;
		tm.tm_min = a->time % 60;
		tm.tm_sec = 0;
		tm.tm_isdst = -1;
		t = mktime(&tm);	/* normalizes tm, so tm_wday is right */
		if (t > from && chumby_alarm_on_day(a, &tm))
			return t;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Shell IPC                                                          */

const char *chumby_run_dir(void)
{
	const char *p = getenv("CHUMBY_RUN_DIR");

	return p && *p ? p : RUN_DIR;
}

/* The shell writes "ringing" or "snoozed <epoch>"; no file = idle. */
chumby_alarm_state_t chumby_alarm_state(time_t *until)
{
	char path[256], word[16];
	long long t = 0;
	chumby_alarm_state_t st = CHUMBY_ALARM_IDLE;
	FILE *f;

	snprintf(path, sizeof(path), "%s/alarm", chumby_run_dir());
	f = fopen(path, "r");
	if (!f)
		return st;
	if (fscanf(f, "%15s %lld", word, &t) >= 1) {
		if (!strcmp(word, "ringing"))
			st = CHUMBY_ALARM_RINGING;
		else if (!strcmp(word, "snoozed"))
			st = CHUMBY_ALARM_SNOOZED;
	}
	fclose(f);
	if (until)
		*until = (time_t)t;
	return st;
}

bool chumby_shell_command(const char *cmd)
{
	char path[256], line[64];
	int fd, len;
	bool ok;

	snprintf(path, sizeof(path), "%s/shell.cmd", chumby_run_dir());
	fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	len = snprintf(line, sizeof(line), "%s\n", cmd);
	ok = write(fd, line, len) == len;
	close(fd);
	return ok;
}
