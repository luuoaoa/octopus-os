/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * liboctopus: common framework for Chumby One apps.
 *
 * Every app (and the shell) gets the same look and behaviour from here:
 *   - a Wayland window driven by LVGL (app_id = the name you pass)
 *   - Rockbox's Nimbus bitmap fonts (the only text font), with LVGL's
 *     symbol font for icons
 *   - the universal dark theme, and night mode switched automatically by
 *     the system policy (/etc/octopus/policy.conf, default 19:00-07:00)
 *   - volume / brightness / screen power helpers and a main loop that can
 *     watch extra fds
 *   - alarms (/var/lib/octopus/alarms.conf), rung by the shell
 *
 * Minimal app:
 *
 *	int main(int argc, char **argv)
 *	{
 *		lv_obj_t *scr = octopus_app_init("Clock");
 *		lv_obj_t *l = lv_label_create(scr);
 *		lv_label_set_text(l, "hello");
 *		return octopus_run();
 *	}
 */
#ifndef OCTOPUS_H
#define OCTOPUS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Screen geometry. octopus-wm picks the UI's size from the screen's (and
 * tells every program it starts, in $OCTOPUS_SCREEN):
 *
 *   320x240, 4:3   the Chumby One, 4:3 screens, small ones
 *   640x360, 16:9  16:9 and 16:10 screens: the same UI at 1.5x, wider
 *
 * Design for 320x240 and write sizes as octopus_px(n): n on 320x240, n
 * times 1.5 (rounded) on 640x360, so 640x360 looks like 320x240 with
 * more width - not a desktop with more of everything. The fonts follow by
 * themselves (one bitmap size up). Lay out against OCTOPUS_APP_W / _H and
 * OCTOPUS_MARGIN, not numbers: they're the screen's.
 */
typedef enum {
	OCTOPUS_ASPECT_4_3,	/* 320x240 */
	OCTOPUS_ASPECT_16_9,	/* 640x360 */
} octopus_aspect_t;

int octopus_screen_w(void);
int octopus_screen_h(void);
octopus_aspect_t octopus_screen_aspect(void);
/* A 320x240 size in this screen's pixels. */
int32_t octopus_px(int32_t px);

/* octopus-wm's tab bar along the top, the app area below. */
#define OCTOPUS_SCREEN_W	octopus_screen_w()
#define OCTOPUS_SCREEN_H	octopus_screen_h()
#define OCTOPUS_BAR_H	octopus_px(32)
#define OCTOPUS_APP_W	OCTOPUS_SCREEN_W
#define OCTOPUS_APP_H	(OCTOPUS_SCREEN_H - OCTOPUS_BAR_H)
/* The shell's volume/brightness slider: drops down from the bar's right
 * end, under their buttons. */
#define OCTOPUS_POPUP_W	octopus_px(216)
#define OCTOPUS_POPUP_H	octopus_px(52)
/* An app's content from the bar and the left and right edges: the same in
 * every app, so nothing jumps when you switch between them. */
#define OCTOPUS_MARGIN	octopus_px(14)

/*
 * Minimum touch target on the resistive panel (~9 mm). Anything a finger
 * must hit should be at least this big in both directions.
 */
#define OCTOPUS_TOUCH_MIN	octopus_px(42)

typedef enum {
	OCTOPUS_MODE_DAY,	/* dark theme */
	OCTOPUS_MODE_NIGHT,	/* darker, warm, low-glare theme */
} octopus_mode_t;

/* The sizes are 320x240's; on 640x360 each is one bitmap size up (14 ->
 * 19, 19 -> 14 at 2x, 35 -> 19 at 2x, 70 -> 35 at 3x). */
typedef enum {
	OCTOPUS_FONT_SMALL,	/* Nimbus 14: captions, status */
	OCTOPUS_FONT_BODY,	/* Nimbus 14: default text */
	OCTOPUS_FONT_LARGE,	/* Nimbus 19: headings */
	OCTOPUS_FONT_XL,		/* Nimbus 35: big values */
	OCTOPUS_FONT_HUGE,	/* Nimbus 35 at 2x: clock */
	OCTOPUS_FONT_MEDIUM,	/* Nimbus 14 at 2x (28 px): mid-size values */
	OCTOPUS_FONT_CAPTION,	/* Nimbus 12: subtext, explanations (smaller than the body) */
	OCTOPUS_FONT_COUNT
} octopus_font_t;

typedef enum {
	OCTOPUS_COLOR_BG,
	OCTOPUS_COLOR_SURFACE,
	OCTOPUS_COLOR_TEXT,
	OCTOPUS_COLOR_TEXT_DIM,
	OCTOPUS_COLOR_ACCENT,
	OCTOPUS_COLOR_BAR,	/* the shell's tab bar, set apart from the app */
	OCTOPUS_COLOR_COUNT
} octopus_color_t;

/* System policy, from /etc/octopus/policy.conf (see policy.conf.example). */
struct octopus_policy {
	int night_start;	/* minutes after midnight, default 19:00 */
	int night_end;		/* minutes after midnight, default 07:00 */
	int force;		/* -1 auto, else an octopus_mode_t */
	int day_brightness;	/* percent, until the popup sets the day's */
	int night_brightness;	/* percent, until the popup sets the night's */
	int screen_off_start;	/* unused: the power button turns the screen */
	int screen_off_end;	/* on and off (kept so old policies parse) */
	int wake_seconds;	/* unused, likewise */
	int snooze_minutes;	/* default 9 */
	int alarm_volume;	/* minimum volume while ringing, percent */
	char saver[32];		/* the screen saver: /usr/lib/octopus/savers/<this>,
				 * "none" for none; default toasters */
	int saver_minutes;	/* without input this long, it starts; default 5 */
};

typedef void (*octopus_mode_cb_t)(octopus_mode_t mode, void *user);
typedef void (*octopus_fd_cb_t)(int fd, void *user);

/* Create the app's window (sized by the compositor) and return its screen. */
lv_obj_t *octopus_app_init(const char *app_id);

/* Lower level, for the shell: framework init, then any number of windows. */
void octopus_init(void);
lv_display_t *octopus_window_create(const char *app_id, int width, int height);
void octopus_window_close(lv_display_t *disp);

/* Run until every window is closed. */
int octopus_run(void);

/* Call `cb` when `fd` becomes readable (inside octopus_run). */
void octopus_watch_fd(int fd, octopus_fd_cb_t cb, void *user);

/*
 * Icons LVGL's symbols lack (Material Icons, fonts/icons2lvgl.py), as
 * UTF-8 strings like LV_SYMBOL_*. They show in octopus_icon_font(px)
 * (14, 20, 28 or 42 for placeholder art: sized like Montserrat), which
 * falls back to LVGL's symbols, so a label in it takes either.
 */
#define OCTOPUS_ICON_RADIO	"\xEE\xA4\x80"	/* U+E900 */
#define OCTOPUS_ICON_CLOCK	"\xEE\xA4\x81"	/* U+E901 */
#define OCTOPUS_ICON_GLOBE	"\xEE\xA4\x82"	/* U+E902 */
#define OCTOPUS_ICON_SUN		"\xEE\xA4\x83"	/* U+E903 */
#define OCTOPUS_ICON_PODCAST	"\xEE\xA4\x84"	/* U+E904 */
#define OCTOPUS_ICON_BACK10	"\xEE\xA4\x85"	/* U+E905 */
#define OCTOPUS_ICON_AHEAD30	"\xEE\xA4\x86"	/* U+E906 */
#define OCTOPUS_ICON_AIRPLAY	"\xEE\xA4\x87"	/* U+E907 */
#define OCTOPUS_ICON_NO_NETWORK	"\xEE\xA4\x88"	/* U+E908 */
const lv_font_t *octopus_icon_font(int px);
/*
 * LVGL's symbols (LV_SYMBOL_*) alone, at a 320x240 size: 14, 20, 28 or 48
 * (Montserrat's), scaled for the screen like octopus_px() - instead of
 * &lv_font_montserrat_<n>, which is the same size on every screen.
 * octopus_icon_font() takes 320x240 sizes too.
 */
const lv_font_t *octopus_symbol_font(int px);

/* Theme */
const lv_font_t *octopus_font(octopus_font_t font);
lv_color_t octopus_color(octopus_color_t color);

/*
 * Colour is for what's active only (the app's tab, play while playing, a
 * selected item, a slider's fill), in the app's own hue: OCTOPUS_COLOR_ACCENT.
 * An app's hue is the "Color = #rrggbb" of its .app (read by
 * octopus_app_init()); the shell's is iPod blue. At night every accent is
 * one warm, dim amber.
 */
#define OCTOPUS_ACCENT_DEFAULT	0x2a7bd6	/* iPod blue */
#define OCTOPUS_ACCENT_NIGHT	0x8c5a1e	/* warm, dim amber */
void octopus_set_accent(uint32_t rgb);
/* Text and icons on an accent fill: white by day, black at night. */
lv_color_t octopus_on_accent(void);
/* Another app's accent (by its day colour), for the current mode. */
lv_color_t octopus_accent(uint32_t day_rgb);
/* Fill an object with a colour, glossy: a touch lighter at the top. */
void octopus_glossy(lv_obj_t *obj, lv_color_t c, lv_style_selector_t sel);
octopus_mode_t octopus_mode(void);
/* Called once now and on every day/night switch: restyle custom objects. */
void octopus_on_mode_change(octopus_mode_cb_t cb, void *user);

/*
 * Layout. The fonts' line boxes are balanced on the capitals (and the
 * icon glyphs on their box), so centring boxes centres what you see.
 * Position things relative to what they belong to, never at a guessed y:
 *
 *  - octopus_group(): a transparent, content-sized flex row or column
 *    whose children are centred on each other (icon + label, number +
 *    caption). Align the group itself like any object.
 *  - octopus_center_y_on/between/at(): move an object vertically so its
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
lv_obj_t *octopus_group(lv_obj_t *parent, lv_flex_flow_t flow, int32_t gap);
void octopus_center_y_on(lv_obj_t *obj, lv_obj_t *ref);
void octopus_center_y_between(lv_obj_t *obj, lv_obj_t *above, lv_obj_t *below);
void octopus_center_y_at(lv_obj_t *obj, int32_t y);

/*
 * Show a PNG or JPEG file (by extension) in an lv_image, scaled to fit a
 * box x box square. JPEGs are decoded whole once and kept (LVGL can't
 * scale them otherwise), again only when the file changes. False when
 * the file can't be shown (missing, unreadable, not an image).
 */
bool octopus_logo_set(lv_obj_t *img, const char *path, int32_t box);

/*
 * Scroll speed for a circular-scrolling label, in px/s, for the text
 * about to be set: a plain duration from the text's width (LVGL's own
 * speed setting caps a pass at ~10 s, too short for long text slowly).
 * Call before lv_label_set_text().
 */
#define OCTOPUS_SCROLL_SPEED	20
void octopus_label_scroll_speed(lv_obj_t *label, const char *text, int px_per_s);

/*
 * One-line labels that, given new text too long for them, scroll through
 * it twice (after a pause, at OCTOPUS_SCROLL_SPEED) and then stop, cut with
 * dots. _init once (after setting the font), then _set the text.
 */
void octopus_scroll_label_init(lv_obj_t *label);
void octopus_scroll_label_set(lv_obj_t *label, const char *text);

/* Policy */
const struct octopus_policy *octopus_policy(void);

/*
 * Settings: /var/lib/octopus/settings.conf, "key = value" lines, the apps'
 * user's. Its keys override the policy's (mode = auto|day|night: night
 * mode), and every app sees a change within 2 s. get(): false if unset.
 * set(): replaces the key's line (val NULL: removes it).
 */
bool octopus_setting_get(const char *key, char *val, size_t size);
bool octopus_setting_set(const char *key, const char *val);

/*
 * The time zone changed (Settings): liboctopus has glibc read it again
 * (localtime_r() follows), within 2 s, and calls cb - to redraw a clock
 * that only redraws when the minute changes.
 */
typedef void (*octopus_zone_cb_t)(void *user);
void octopus_on_zone_change(octopus_zone_cb_t cb, void *user);
/* Is the local time now in [start, end) (minutes, may wrap midnight)? */
bool octopus_in_window(int start, int end);
/*
 * Is the clock set? There's no RTC battery: after a power cut the time is
 * wrong (1970, or timesyncd's last saved time) until NTP syncs. True once
 * timesyncd has synchronised, or when the RTC holds a real date (it kept
 * running across a reboot). Time-of-day policy (night mode, screen off,
 * alarms) must not act on a wrong clock.
 */
bool octopus_time_valid(void);

/*
 * Has timesyncd set the clock from the internet this boot? Stricter than
 * octopus_time_valid(): alarms wait for it, and the clock shows a "no
 * network" icon until then.
 */
bool octopus_time_synced(void);

/* Hardware helpers (percent, 0..100; -1 if unavailable) */
int octopus_volume_get(void);
void octopus_volume_set(int percent);
int octopus_brightness_get(void);
void octopus_brightness_set(int percent);
/*
 * Backlight power: off is completely dark (the brightness is kept). Also
 * sets the CPU governor: performance while lit, powersave while dark - the
 * panel glitches whenever the CPU (and with it the AHB the LCD controller
 * reads from) changes frequency, which nobody sees with the screen off.
 */
void octopus_screen_power(bool on);
bool octopus_screen_is_on(void);

/* Run a command in the background (no shell); returns the pid or -1. */
int octopus_spawn(const char *path, char *const argv[]);

/*
 * Alarms. /var/lib/octopus/alarms.conf, one per line: "06:30 MTWTF-- on"
 * (days Monday..Sunday, '-' = not that day, "once" = next occurrence
 * only). The clock app edits them; the shell rings them, so they go off
 * whatever app is open.
 */
#define OCTOPUS_MAX_ALARMS	8
#define OCTOPUS_DAYS_ALL		0x7f
#define OCTOPUS_DAYS_WEEKDAYS	0x1f
#define OCTOPUS_DAYS_WEEKEND	0x60

struct octopus_alarm {
	int time;		/* minutes after midnight */
	unsigned char days;	/* bit 0 = Monday .. bit 6 = Sunday; 0 = once */
	bool enabled;
};

const char *octopus_alarms_path(void);
int octopus_alarms_load(struct octopus_alarm *alarms, int max);
bool octopus_alarms_save(const struct octopus_alarm *alarms, int n);
/* "Every day", "Weekdays", "Weekends", "Once" or e.g. "Mon Wed Fri". */
const char *octopus_alarm_days_text(unsigned char days, char *buf, size_t len);
/* Does it ring on the day of `tm`? */
bool octopus_alarm_on_day(const struct octopus_alarm *a, const struct tm *tm);
/* When an enabled alarm next rings after `from` (0 if disabled). */
time_t octopus_alarm_next(const struct octopus_alarm *a, time_t from);

/* What the shell's alarm is doing (it publishes this in the run dir). */
typedef enum {
	OCTOPUS_ALARM_IDLE,
	OCTOPUS_ALARM_RINGING,
	OCTOPUS_ALARM_SNOOZED,	/* *until = when it rings again */
} octopus_alarm_state_t;

octopus_alarm_state_t octopus_alarm_state(time_t *until);

/*
 * Runtime directory shared with the shell (/run/octopus, or
 * $OCTOPUS_RUN_DIR). The shell reads commands from its "shell.cmd" FIFO:
 * "snooze" (= the snooze button: snoozes a ringing alarm), "power" (= the
 * power button: the screen off or on), "screen on" / "screen off",
 * "stop" (dismiss the alarm), "ring" (ring now, for testing), "volume" /
 * "brightness" (open the popup), "menu" (the app menu), "calibrate" (the
 * touchscreen calibration), "launch <Id>" (an app to the front).
 */
const char *octopus_run_dir(void);
bool octopus_shell_command(const char *cmd);

/*
 * octopus-wm freezes (SIGSTOP) apps that aren't in front, until they're
 * brought back. An app with work to do out of sight (a download, a
 * timer that must fire) asks to keep running meanwhile, and lets go when
 * done: stay_awake(true) marks this process ($run_dir/awake/<pid>), which
 * octopus-wm checks before freezing; false (or exiting) removes the mark.
 * Alarms don't need it: the shell rings them, and brings the clock up.
 */
void octopus_stay_awake(bool on);

/*
 * An installed bundle (octopus-install, /var/apps/<id>): its directory
 * (read-only: its pictures, sounds, data files), and one of its own to
 * write in (/var/lib/octopus/apps/<id>, kept across updates and launches).
 * NULL for the image's own apps.
 */
const char *octopus_app_dir(void);
const char *octopus_app_data_dir(void);

/*
 * The stream player, octopus-stream: one stream for the whole device (as
 * there's one speaker), playing in its own process - so it goes on while
 * the app is frozen, closed or evicted. A URL can be an Icecast/Shoutcast
 * stream, an HLS playlist, a .pls/.m3u, or a file (MP3, .m4a).
 *
 * play(): starts it (stopping whatever played: FM, AirPlay, another
 * stream), in the background, the call returning at once; name is what to
 * call it (shown by the volume control's stop button). stop(): stops it.
 * status(): false if nothing plays (or played to its end: state "ended");
 * else what plays.
 */
struct octopus_stream_status {
	char state[16];		/* "connecting", "playing", "error", "ended" */
	char url[256];
	char name[64];		/* as given to play() */
	char title[128];	/* what plays now, from the stream ("" if it says nothing) */
	int bitrate;		/* kbit/s, measured (0 until known) */
	int played_s;		/* a file: seconds played (0 for a live stream) */
	long length;		/* a file: its size in bytes (0 for a live stream) */
	long byte;		/* a file: how far in, in bytes */
};

/*
 * USB sticks: each one plugged in is mounted read-only (FAT, or exFAT)
 * in a directory of its own under this one (/run/octopus-usb/sda1...,
 * or $OCTOPUS_USB_DIR), readable by the apps, until it's pulled out.
 * The directory is always there: watch it (inotify) for sticks coming
 * and going. A file on one plays with octopus_stream_play(path, name).
 */
const char *octopus_usb_dir(void);

/*
 * A music file's tags (ID3v2 or ID3v1, as MP3s carry them), as UTF-8;
 * "" for those it hasn't. False if it has none at all (or can't be
 * read): show its file name instead.
 */
struct octopus_tags {
	char title[128];
	char artist[128];
	char album[128];
	int track;		/* its number on the album, 0 if not given */
};
bool octopus_audio_tags(const char *path, struct octopus_tags *tags);

bool octopus_stream_play(const char *url, const char *name);
bool octopus_stream_stop(void);
bool octopus_stream_status(struct octopus_stream_status *st);

#ifdef __cplusplus
}
#endif

#endif /* OCTOPUS_H */
