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
 *   - alarms (/etc/chumby/alarms.conf), rung by the shell
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

/* Screen geometry: chumby-wm's left panel and the app area beside it. */
#define CHUMBY_SCREEN_W	320
#define CHUMBY_SCREEN_H	240
#define CHUMBY_PANEL_W	64
#define CHUMBY_APP_W	(CHUMBY_SCREEN_W - CHUMBY_PANEL_W)

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
	CHUMBY_FONT_COUNT
} chumby_font_t;

typedef enum {
	CHUMBY_COLOR_BG,
	CHUMBY_COLOR_SURFACE,
	CHUMBY_COLOR_TEXT,
	CHUMBY_COLOR_TEXT_DIM,
	CHUMBY_COLOR_ACCENT,
	CHUMBY_COLOR_COUNT
} chumby_color_t;

/* System policy, from /etc/chumby/policy.conf (see policy.conf.example). */
struct chumby_policy {
	int night_start;	/* minutes after midnight, default 19:00 */
	int night_end;		/* minutes after midnight, default 07:00 */
	int force;		/* -1 auto, else a chumby_mode_t */
	int day_brightness;	/* percent */
	int night_brightness;	/* percent */
	int screen_off_start;	/* backlight off from here, default 21:00 */
	int screen_off_end;	/* ... to here, default 07:00 (same = never) */
	int wake_seconds;	/* snooze button lights it this long, default 10 */
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

/* Theme */
const lv_font_t *chumby_font(chumby_font_t font);
lv_color_t chumby_color(chumby_color_t color);
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

/* Policy */
const struct chumby_policy *chumby_policy(void);
/* Is the local time now in [start, end) (minutes, may wrap midnight)? */
bool chumby_in_window(int start, int end);
/*
 * Is the clock set? There's no RTC battery: after a power cut the time
 * starts in 1970 until NTP syncs. Time-of-day policy (night mode, screen
 * off, alarms) must not act on that.
 */
bool chumby_time_valid(void);

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
 * Alarms. /etc/chumby/alarms.conf, one per line: "06:30 MTWTF-- on"
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
 * (ring now, for testing), "volume" / "brightness" (open the popup).
 */
const char *chumby_run_dir(void);
bool chumby_shell_command(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* CHUMBY_H */
