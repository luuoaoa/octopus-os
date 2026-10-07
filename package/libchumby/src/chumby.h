/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libchumby: common framework for Chumby One apps.
 *
 * Every app (and the shell) gets the same look and behaviour from here:
 *   - a Wayland window driven by LVGL (app_id = the name you pass)
 *   - Noto Sans fonts (FreeType), with LVGL symbols as fallback
 *   - the universal dark theme, and night mode switched automatically by
 *     the system policy (/etc/chumby/policy.conf, default 19:00-07:00)
 *   - volume / brightness helpers and a main loop that can watch extra fds
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
	CHUMBY_FONT_SMALL,	/* 13 px: captions, status */
	CHUMBY_FONT_BODY,	/* 15 px: default text */
	CHUMBY_FONT_LARGE,	/* 24 px bold: headings */
	CHUMBY_FONT_XL,		/* 34 px bold: big values */
	CHUMBY_FONT_HUGE,	/* 56 px bold: clock */
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

/* Policy */
const struct chumby_policy *chumby_policy(void);

/* Hardware helpers (percent, 0..100; -1 if unavailable) */
int chumby_volume_get(void);
void chumby_volume_set(int percent);
int chumby_brightness_get(void);
void chumby_brightness_set(int percent);

/* Run a command in the background (no shell); returns the pid or -1. */
int chumby_spawn(const char *path, char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif /* CHUMBY_H */
