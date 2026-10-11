/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libchumby: common framework for Chumby One apps.
 *
 * Every app (and the shell) gets the same look and behaviour from here:
 *   - a Wayland window driven by LVGL (app_id = the name you pass)
 *   - Rockbox's Nimbus bitmap fonts (the only text font), with LVGL's
 *     symbol font for icons
 *   - the universal dark theme, and night mode switched automatically by
 *     the system policy (/etc/chumby/policy.conf, default 19:00-07:00)
 *   - volume / brightness / screen power helpers and a main loop that can
 *     watch extra fds
 *   - alarms (/var/lib/chumby/alarms.conf), rung by the shell
 *
 * Minimal app:
 *
 *	int main(int argc, char **argv)
 *	{
 *		lv_obj_t *scr = chumby_app_init("Clock");
 *		lv_obj_t *l = lv_label_create(scr);
 *		lv_label_set_text(l, "hello");
 *		return chumby_run();
 *	}
 */
#ifndef CHUMBY_H
#define CHUMBY_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Screen geometry: chumby-wm's tab bar along the top, the app area below. */
#define CHUMBY_SCREEN_W	320
#define CHUMBY_SCREEN_H	240
#define CHUMBY_BAR_H	32
#define CHUMBY_APP_W	CHUMBY_SCREEN_W
#define CHUMBY_APP_H	(CHUMBY_SCREEN_H - CHUMBY_BAR_H)
/* The shell's volume/brightness slider: drops down from the bar's right
 * end, under their buttons. */
#define CHUMBY_POPUP_W	216
#define CHUMBY_POPUP_H	52

/*
 * Minimum touch target on the resistive panel (~9 mm). Anything a finger
 * must hit should be at least this big in both directions.
 */
#define CHUMBY_TOUCH_MIN	42

typedef enum {
	CHUMBY_MODE_DAY,	/* dark theme */
	CHUMBY_MODE_NIGHT,	/* darker, warm, low-glare theme */
} chumby_mode_t;

typedef enum {
	CHUMBY_FONT_SMALL,	/* Nimbus 14: captions, status */
	CHUMBY_FONT_BODY,	/* Nimbus 14: default text */
	CHUMBY_FONT_LARGE,	/* Nimbus 19: headings */
	CHUMBY_FONT_XL,		/* Nimbus 35: big values */
	CHUMBY_FONT_HUGE,	/* Nimbus 35 at 2x: clock */
	CHUMBY_FONT_MEDIUM,	/* Nimbus 14 at 2x (28 px): mid-size values */
	CHUMBY_FONT_COUNT
} chumby_font_t;

typedef enum {
	CHUMBY_COLOR_BG,
	CHUMBY_COLOR_SURFACE,
	CHUMBY_COLOR_TEXT,
	CHUMBY_COLOR_TEXT_DIM,
	CHUMBY_COLOR_ACCENT,
	CHUMBY_COLOR_BAR,	/* the shell's tab bar, set apart from the app */
	CHUMBY_COLOR_COUNT
} chumby_color_t;

/* System policy, from /etc/chumby/policy.conf (see policy.conf.example). */
struct chumby_policy {
	int night_start;	/* minutes after midnight, default 19:00 */
	int night_end;		/* minutes after midnight, default 07:00 */
	int force;		/* -1 auto, else a chumby_mode_t */
	int day_brightness;	/* percent, until the popup sets the day's */
	int night_brightness;	/* percent, until the popup sets the night's */
	int screen_off_start;	/* unused: the power button turns the screen */
	int screen_off_end;	/* on and off (kept so old policy files parse) */
	int wake_seconds;	/* unused, likewise */
	int snooze_minutes;	/* default 9 */
	int alarm_volume;	/* minimum volume while ringing, percent */
};

typedef void (*chumby_mode_cb_t)(chumby_mode_t mode, void *user);
typedef void (*chumby_fd_cb_t)(int fd, void *user);

/* Create the app's window (sized by the compositor) and return its screen. */
lv_obj_t *chumby_app_init(const char *app_id);

/* Lower level, for the shell: framework init, then any number of windows. */
void chumby_init(void);
lv_display_t *chumby_window_create(const char *app_id, int width, int height);
void chumby_window_close(lv_display_t *disp);

/* Run until every window is closed. */
int chumby_run(void);

/* Call `cb` when `fd` becomes readable (inside chumby_run). */
void chumby_watch_fd(int fd, chumby_fd_cb_t cb, void *user);

/*
 * Icons LVGL's symbols lack (Material Icons, fonts/icons2lvgl.py), as
 * UTF-8 strings like LV_SYMBOL_*. They show in chumby_icon_font(px)
 * (14, 20, 28 or 42 for placeholder art: sized like Montserrat), which
 * falls back to LVGL's symbols, so a label in it takes either.
 */
#define CHUMBY_ICON_RADIO	"\xEE\xA4\x80"	/* U+E900 */
#define CHUMBY_ICON_CLOCK	"\xEE\xA4\x81"	/* U+E901 */
#define CHUMBY_ICON_GLOBE	"\xEE\xA4\x82"	/* U+E902 */
#define CHUMBY_ICON_SUN		"\xEE\xA4\x83"	/* U+E903 */
#define CHUMBY_ICON_PODCAST	"\xEE\xA4\x84"	/* U+E904 */
#define CHUMBY_ICON_BACK10	"\xEE\xA4\x85"	/* U+E905 */
#define CHUMBY_ICON_AHEAD30	"\xEE\xA4\x86"	/* U+E906 */
#define CHUMBY_ICON_AIRPLAY	"\xEE\xA4\x87"	/* U+E907 */
#define CHUMBY_ICON_NO_NETWORK	"\xEE\xA4\x88"	/* U+E908 */
const lv_font_t *chumby_icon_font(int px);

/* Theme */
const lv_font_t *chumby_font(chumby_font_t font);
lv_color_t chumby_color(chumby_color_t color);

/*
 * Colour is for what's active only (the app's tab, play while playing, a
 * selected item, a slider's fill), in the app's own hue: CHUMBY_COLOR_ACCENT.
 * An app's hue is the "Color = #rrggbb" of its .app (read by
 * chumby_app_init()); the shell's is iPod blue. At night every accent is
 * one warm, dim amber.
 */
#define CHUMBY_ACCENT_DEFAULT	0x2a7bd6	/* iPod blue */
#define CHUMBY_ACCENT_NIGHT	0x8c5a1e	/* warm, dim amber */
void chumby_set_accent(uint32_t rgb);
/* Text and icons on an accent fill: white by day, black at night. */
lv_color_t chumby_on_accent(void);
/* Another app's accent (by its day colour), for the current mode. */
lv_color_t chumby_accent(uint32_t day_rgb);
/* Fill an object with a colour, glossy: a touch lighter at the top. */
void chumby_glossy(lv_obj_t *obj, lv_color_t c, lv_style_selector_t sel);
chumby_mode_t chumby_mode(void);
/* Called once now and on every day/night switch: restyle custom objects. */
void chumby_on_mode_change(chumby_mode_cb_t cb, void *user);

/*
 * Layout. The fonts' line boxes are balanced on the capitals (and the
 * icon glyphs on their box), so centring boxes centres what you see.
 * Position things relative to what they belong to, never at a guessed y:
 *
 *  - chumby_group(): a transparent, content-sized flex row or column
 *    whose children are centred on each other (icon + label, number +
 *    caption). Align the group itself like any object.
 *  - chumby_center_y_on/between/at(): move an object vertically so its
 *    *visible* centre is on another object's, midway between two
 *    objects, or at a y in its parent. Visible means: text from the top
 *    of its capitals to its baseline, icons and lone punctuation (":")
 *    by their glyphs, a group by its children - so a big clock over a
 *    small date centres as you see it, not by the empty line padding.
 *    They only change the y offset, so an LV_ALIGN_*_MID / RIGHT
 *    alignment keeps working when text changes. One-shot: call once the
 *    reference objects are placed (an empty label measures like text).
 *    Not for children of a flex container (the layout owns those).
 */
lv_obj_t *chumby_group(lv_obj_t *parent, lv_flex_flow_t flow, int32_t gap);
void chumby_center_y_on(lv_obj_t *obj, lv_obj_t *ref);
void chumby_center_y_between(lv_obj_t *obj, lv_obj_t *above, lv_obj_t *below);
void chumby_center_y_at(lv_obj_t *obj, int32_t y);

/*
 * Show a PNG or JPEG file (by extension) in an lv_image, scaled to fit a
 * box x box square. JPEGs are decoded whole once and kept (LVGL can't
 * scale them otherwise), again only when the file changes. False when
 * the file can't be shown (missing, unreadable, not an image).
 */
bool chumby_logo_set(lv_obj_t *img, const char *path, int32_t box);

/*
 * Scroll speed for a circular-scrolling label, in px/s, for the text
 * about to be set: a plain duration from the text's width (LVGL's own
 * speed setting caps a pass at ~10 s, too short for long text slowly).
 * Call before lv_label_set_text().
 */
#define CHUMBY_SCROLL_SPEED	20
void chumby_label_scroll_speed(lv_obj_t *label, const char *text, int px_per_s);

/*
 * One-line labels that, given new text too long for them, scroll through
 * it twice (after a pause, at CHUMBY_SCROLL_SPEED) and then stop, cut with
 * dots. _init once (after setting the font), then _set the text.
 */
void chumby_scroll_label_init(lv_obj_t *label);
void chumby_scroll_label_set(lv_obj_t *label, const char *text);

/* Policy */
const struct chumby_policy *chumby_policy(void);
/* Is the local time now in [start, end) (minutes, may wrap midnight)? */
bool chumby_in_window(int start, int end);
/*
 * Is the clock set? There's no RTC battery: after a power cut the time is
 * wrong (1970, or timesyncd's last saved time) until NTP syncs. True once
 * timesyncd has synchronised, or when the RTC holds a real date (it kept
 * running across a reboot). Time-of-day policy (night mode, screen off,
 * alarms) must not act on a wrong clock.
 */
bool chumby_time_valid(void);

/*
 * Has timesyncd set the clock from the internet this boot? Stricter than
 * chumby_time_valid(): alarms wait for it, and the clock shows a "no
 * network" icon until then.
 */
bool chumby_time_synced(void);

/* Hardware helpers (percent, 0..100; -1 if unavailable) */
int chumby_volume_get(void);
void chumby_volume_set(int percent);
int chumby_brightness_get(void);
void chumby_brightness_set(int percent);
/*
 * Backlight power: off is completely dark (the brightness is kept). Also
 * sets the CPU governor: performance while lit, powersave while dark - the
 * panel glitches whenever the CPU (and with it the AHB the LCD controller
 * reads from) changes frequency, which nobody sees with the screen off.
 */
void chumby_screen_power(bool on);
bool chumby_screen_is_on(void);

/* Run a command in the background (no shell); returns the pid or -1. */
int chumby_spawn(const char *path, char *const argv[]);

/*
 * Alarms. /var/lib/chumby/alarms.conf, one per line: "06:30 MTWTF-- on"
 * (days Monday..Sunday, '-' = not that day, "once" = next occurrence
 * only). The clock app edits them; the shell rings them, so they go off
 * whatever app is open.
 */
#define CHUMBY_MAX_ALARMS	8
#define CHUMBY_DAYS_ALL		0x7f
#define CHUMBY_DAYS_WEEKDAYS	0x1f
#define CHUMBY_DAYS_WEEKEND	0x60

struct chumby_alarm {
	int time;		/* minutes after midnight */
	unsigned char days;	/* bit 0 = Monday .. bit 6 = Sunday; 0 = once */
	bool enabled;
};

const char *chumby_alarms_path(void);
int chumby_alarms_load(struct chumby_alarm *alarms, int max);
bool chumby_alarms_save(const struct chumby_alarm *alarms, int n);
/* "Every day", "Weekdays", "Weekends", "Once" or e.g. "Mon Wed Fri". */
const char *chumby_alarm_days_text(unsigned char days, char *buf, size_t len);
/* Does it ring on the day of `tm`? */
bool chumby_alarm_on_day(const struct chumby_alarm *a, const struct tm *tm);
/* When an enabled alarm next rings after `from` (0 if disabled). */
time_t chumby_alarm_next(const struct chumby_alarm *a, time_t from);

/* What the shell's alarm is doing (it publishes this in the run dir). */
typedef enum {
	CHUMBY_ALARM_IDLE,
	CHUMBY_ALARM_RINGING,
	CHUMBY_ALARM_SNOOZED,	/* *until = when it rings again */
} chumby_alarm_state_t;

chumby_alarm_state_t chumby_alarm_state(time_t *until);

/*
 * Runtime directory shared with the shell (/run/chumby, or
 * $CHUMBY_RUN_DIR). The shell reads commands from its "shell.cmd" FIFO:
 * "snooze" (= the snooze button), "stop" (dismiss the alarm), "ring"
 * (ring now, for testing), "volume" / "brightness" (open the popup),
 * "menu" (the app menu), "calibrate" (the touchscreen calibration).
 */
const char *chumby_run_dir(void);
bool chumby_shell_command(const char *cmd);

/*
 * chumby-wm freezes (SIGSTOP) apps that aren't in front, until they're
 * brought back. An app with work to do out of sight (a download, a
 * timer that must fire) asks to keep running meanwhile, and lets go when
 * done: stay_awake(true) marks this process ($run_dir/awake/<pid>), which
 * chumby-wm checks before freezing; false (or exiting) removes the mark.
 * Alarms don't need it: the shell rings them, and brings the clock up.
 */
void chumby_stay_awake(bool on);

#ifdef __cplusplus
}
#endif

#endif /* CHUMBY_H */
