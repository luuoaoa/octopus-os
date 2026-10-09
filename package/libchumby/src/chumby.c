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
/* chumby_logo_set() decodes JPEGs by hand. */
#include "lvgl/src/image/lv_image_decoder_private.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

extern char **environ;

#define POLICY_FILE	"/etc/chumby/policy.conf"
#define ALARMS_FILE	"/var/lib/chumby/alarms.conf"
#define RUN_DIR		"/run/chumby"
#define BACKLIGHT	"/sys/class/backlight/backlight/"
#define CPU_GOVERNOR	"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
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

/*
 * No RTC battery: after a power cut the RTC restarts near 1970, and
 * timesyncd moves the system clock to its last saved time - plausible but
 * stale - until it reaches a server. So trust timesyncd's sync flag, or
 * the RTC itself: it only holds a real date if it kept running (a reboot)
 * or the kernel wrote synchronised time into it (CONFIG_RTC_SYSTOHC).
 */
#define TIME_VALID_AFTER	1704067200	/* 2024-01-01 */

bool chumby_time_valid(void)
{
	long long rtc = 0;
	FILE *f;

	if (access("/run/systemd/timesync/synchronized", F_OK) == 0)
		return true;
	f = fopen("/sys/class/rtc/rtc0/since_epoch", "r");
	if (!f)
		return time(NULL) >= TIME_VALID_AFTER;	/* no RTC: best guess */
	if (fscanf(f, "%lld", &rtc) != 1)
		rtc = 0;
	fclose(f);
	return rtc >= TIME_VALID_AFTER;
}

static chumby_mode_t policy_mode_now(void)
{
	if (policy.force >= 0)
		return policy.force;
	if (!chumby_time_valid())
		return CHUMBY_MODE_DAY;
	return chumby_in_window(policy.night_start, policy.night_end) ?
		CHUMBY_MODE_NIGHT : CHUMBY_MODE_DAY;
}

/* ------------------------------------------------------------------ */
/* Theme                                                              */

static chumby_mode_t mode = CHUMBY_MODE_DAY;

/* Shades of grey. Day: dark on light. Night: light on dark, the text a
 * soft grey rather than white so it doesn't light up a bedroom. */
static const uint32_t palette[2][CHUMBY_COLOR_COUNT] = {
	[CHUMBY_MODE_DAY] = {
		[CHUMBY_COLOR_BG]	= 0xe4e4e4,
		[CHUMBY_COLOR_SURFACE]	= 0xf5f5f5,
		[CHUMBY_COLOR_TEXT]	= 0x1c1c1c,
		[CHUMBY_COLOR_TEXT_DIM]	= 0x6e6e6e,
		[CHUMBY_COLOR_ACCENT]	= 0x4a4a4a,
	},
	[CHUMBY_MODE_NIGHT] = {
		[CHUMBY_COLOR_BG]	= 0x000000,
		[CHUMBY_COLOR_SURFACE]	= 0x1e1e1e,
		[CHUMBY_COLOR_TEXT]	= 0xc8c8c8,
		[CHUMBY_COLOR_TEXT_DIM]	= 0x6a6a6a,
		[CHUMBY_COLOR_ACCENT]	= 0x7a7a7a,
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

/* Rockbox's Nimbus bitmap fonts, generated from the BDFs in fonts/ at build time. */
extern lv_font_t nimbus_14, nimbus_19, nimbus_35, nimbus_35x2, nimbus_14x2;

static const lv_font_t *fonts[CHUMBY_FONT_COUNT];

static const lv_font_t *nimbus(lv_font_t *f, const lv_font_t *symbols)
{
	/* Nimbus has no LV_SYMBOL_* icons: those come from LVGL's own font. */
	f->fallback = symbols;
	return f;
}

static void fonts_load(void)
{
	fonts[CHUMBY_FONT_SMALL] = nimbus(&nimbus_14, &lv_font_montserrat_14);
	fonts[CHUMBY_FONT_BODY] = nimbus(&nimbus_14, &lv_font_montserrat_14);
	fonts[CHUMBY_FONT_LARGE] = nimbus(&nimbus_19, &lv_font_montserrat_20);
	fonts[CHUMBY_FONT_XL] = nimbus(&nimbus_35, &lv_font_montserrat_28);
	fonts[CHUMBY_FONT_HUGE] = nimbus(&nimbus_35x2, &lv_font_montserrat_28);
	fonts[CHUMBY_FONT_MEDIUM] = nimbus(&nimbus_14x2, &lv_font_montserrat_28);
}

const lv_font_t *chumby_font(chumby_font_t f)
{
	return fonts[f < CHUMBY_FONT_COUNT ? f : CHUMBY_FONT_BODY];
}

/* ------------------------------------------------------------------ */
/* Layout                                                             */

static int group_tag;	/* user_data marker of chumby_group() objects */

lv_obj_t *chumby_group(lv_obj_t *parent, lv_flex_flow_t flow, int32_t gap)
{
	lv_obj_t *g = lv_obj_create(parent);

	lv_obj_remove_style_all(g);
	lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(g, flow);
	lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(g, gap, 0);
	/* Taps go to whatever the group sits in (e.g. a list row). */
	lv_obj_set_clickable(g, false);
	lv_obj_set_scrollable(g, false);
	lv_obj_set_user_data(g, &group_tag);
	return g;
}

/* Next code point of a UTF-8 string, advancing *i; 0 at the end. */
static uint32_t utf8_next(const char *s, uint32_t *i)
{
	const unsigned char *p = (const unsigned char *)s + *i;
	uint32_t cp;
	int n;

	if (!*p)
		return 0;
	if (*p < 0x80) {
		(*i)++;
		return *p;
	}
	n = *p >= 0xf0 ? 3 : *p >= 0xe0 ? 2 : 1;
	cp = *p & (0x3f >> n);
	while (n-- && (*++p & 0xc0) == 0x80)
		cp = cp << 6 | (*p & 0x3f);
	*i = (uint32_t)(p - (const unsigned char *)s) + (n < 0);
	return cp;
}

static bool is_icon(uint32_t cp)
{
	return cp >= 0xe000 && cp <= 0xf8ff;	/* LV_SYMBOL_* live here */
}

static bool is_alnum(uint32_t cp)
{
	return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
	       (cp >= 'a' && cp <= 'z') || (cp >= 0xc0 && !is_icon(cp));
}

/*
 * What a label visibly occupies, in pixels above its baseline: text is
 * capital height down to the baseline (descenders don't count, and an
 * empty label counts as text, so it can be placed before its text is
 * set); icons and punctuation-only text count by their actual glyphs.
 */
static void label_ink(lv_obj_t *label, int32_t *top, int32_t *bottom)
{
	const lv_font_t *f = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	const char *txt = lv_label_get_text(label);
	bool text = true;
	lv_font_glyph_dsc_t g;
	uint32_t i = 0, cp;

	*top = INT32_MIN;
	*bottom = INT32_MAX;
	if (txt && *txt) {
		text = false;
		while ((cp = utf8_next(txt, &i)) != 0)
			text |= is_alnum(cp);
	}
	if (text && lv_font_get_glyph_dsc(f, &g, 'H', 0)) {
		*top = g.ofs_y + g.box_h;
		*bottom = 0;
	}
	for (i = 0; txt && (cp = utf8_next(txt, &i)) != 0; ) {
		if (is_alnum(cp) || !lv_font_get_glyph_dsc(f, &g, cp, 0) || !g.box_h)
			continue;
		if (text && !is_icon(cp))
			continue;	/* punctuation inside text: cap box rules */
		*top = LV_MAX(*top, g.ofs_y + g.box_h);
		*bottom = LV_MIN(*bottom, g.ofs_y);
	}
}

/* The visible extent of obj: labels by their ink, groups by their
 * children's, anything else by its box. */
static void visual_area(lv_obj_t *obj, lv_area_t *a)
{
	lv_obj_get_coords(obj, a);
	if (lv_obj_check_type(obj, &lv_label_class)) {
		const lv_font_t *f = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
		int32_t top, bottom;
		lv_area_t c;

		label_ink(obj, &top, &bottom);
		if (top == INT32_MIN)
			return;
		/* First line's caps to last line's baseline. */
		lv_obj_get_content_coords(obj, &c);
		a->y1 = c.y1 + f->line_height - f->base_line - top;
		a->y2 = c.y2 + 1 - f->base_line - bottom - 1;
	} else if (lv_obj_get_user_data(obj) == &group_tag &&
		   lv_obj_get_child_count(obj)) {
		uint32_t i;

		a->y1 = INT32_MAX;
		a->y2 = INT32_MIN;
		for (i = 0; i < lv_obj_get_child_count(obj); i++) {
			lv_area_t ca;

			visual_area(lv_obj_get_child(obj, i), &ca);
			a->y1 = LV_MIN(a->y1, ca.y1);
			a->y2 = LV_MAX(a->y2, ca.y2);
		}
	}
}

static int32_t center_y_of(lv_obj_t *obj)
{
	lv_area_t a;

	visual_area(obj, &a);
	return (a.y1 + a.y2 + 1) / 2;
}

/* Shift obj's y offset so its visible centre lands on absolute y `cy`. */
static void center_y_abs(lv_obj_t *obj, int32_t cy)
{
	lv_obj_update_layout(obj);
	lv_obj_set_y(obj, lv_obj_get_style_y(obj, LV_PART_MAIN) + cy - center_y_of(obj));
}

void chumby_center_y_on(lv_obj_t *obj, lv_obj_t *ref)
{
	lv_obj_update_layout(ref);
	center_y_abs(obj, center_y_of(ref));
}

void chumby_center_y_between(lv_obj_t *obj, lv_obj_t *above, lv_obj_t *below)
{
	lv_area_t a, b;

	lv_obj_update_layout(above);
	visual_area(above, &a);
	visual_area(below, &b);
	center_y_abs(obj, (a.y2 + 1 + b.y1) / 2);
}

void chumby_center_y_at(lv_obj_t *obj, int32_t y)
{
	lv_area_t p;

	lv_obj_update_layout(obj);
	lv_obj_get_coords(lv_obj_get_parent(obj), &p);
	center_y_abs(obj, p.y1 + y);
}

/* ------------------------------------------------------------------ */
/* Logos                                                              */

/* LVGL decodes JPEGs (TJPGD) one MCU at a time and can't scale images
 * decoded that way, so decode the whole image into a buffer once. */
static lv_draw_buf_t *decode_tiles(const char *src)
{
	lv_image_decoder_dsc_t dsc;
	lv_area_t full, part;
	lv_draw_buf_t *buf = NULL;
	int32_t w, h, y;

	if (lv_image_decoder_open(&dsc, src, NULL) != LV_RESULT_OK)
		return NULL;
	w = dsc.header.w;
	h = dsc.header.h;
	if (dsc.decoded || dsc.header.cf != LV_COLOR_FORMAT_RGB888 ||
	    !(buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB888, LV_STRIDE_AUTO)))
		goto out;
	lv_area_set(&full, 0, 0, w - 1, h - 1);
	part.x1 = part.y1 = part.x2 = part.y2 = LV_COORD_MIN;
	do {
		if (lv_image_decoder_get_area(&dsc, &full, &part) != LV_RESULT_OK) {
			lv_draw_buf_destroy(buf);
			buf = NULL;
			goto out;
		}
		for (y = part.y1; y <= part.y2; y++)
			memcpy(buf->data + y * buf->header.stride + part.x1 * 3,
			       dsc.decoded->data + (y - part.y1) * dsc.decoded->header.stride,
			       lv_area_get_width(&part) * 3);
	} while (part.x2 < w - 1 || part.y2 < h - 1);
out:
	lv_image_decoder_close(&dsc);
	return buf;
}

#define MAX_LOGOS	4

/* Per image object: its source, and a decoded JPEG it shows. */
static struct logo {
	lv_obj_t *img;
	char src[256];
	time_t mtime;
	lv_draw_buf_t *jpeg;
} logos[MAX_LOGOS];

static void logo_drop(struct logo *l)
{
	if (l->jpeg) {
		if (lv_image_get_src(l->img) == l->jpeg)
			lv_image_set_src(l->img, NULL);
		lv_image_cache_drop(l->jpeg);
		lv_draw_buf_destroy(l->jpeg);
	}
	l->jpeg = NULL;
}

static void logo_deleted(lv_event_t *e)
{
	struct logo *l = lv_event_get_user_data(e);

	if (l->jpeg) {
		lv_image_cache_drop(l->jpeg);
		lv_draw_buf_destroy(l->jpeg);
	}
	memset(l, 0, sizeof(*l));
}

bool chumby_logo_set(lv_obj_t *img, const char *path, int32_t box)
{
	struct logo *l = NULL;
	const char *ext = strrchr(path, '.');
	char src[256];
	lv_image_header_t h;
	struct stat st;
	int32_t sw, sh;
	int i;

	for (i = 0; i < MAX_LOGOS && !l; i++)
		if (logos[i].img == img)
			l = &logos[i];
	for (i = 0; i < MAX_LOGOS && !l; i++)
		if (!logos[i].img) {
			l = &logos[i];
			l->img = img;
			lv_obj_add_event_cb(img, logo_deleted, LV_EVENT_DELETE, l);
		}
	if (!l || stat(path, &st))
		return false;
	snprintf(src, sizeof(src), "A:%s", path);
	if (ext && (!strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg"))) {
		/* Decode again only when it's another file, or got replaced. */
		if (!l->jpeg || strcmp(l->src, src) || st.st_mtime != l->mtime) {
			logo_drop(l);
			l->jpeg = decode_tiles(src);
			snprintf(l->src, sizeof(l->src), "%s", src);
			l->mtime = st.st_mtime;
		}
		if (!l->jpeg)
			return false;
		h = l->jpeg->header;
		lv_image_set_src(img, l->jpeg);
	} else {
		if (lv_image_decoder_get_info(src, &h) != LV_RESULT_OK || !h.w || !h.h)
			return false;
		logo_drop(l);
		snprintf(l->src, sizeof(l->src), "%s", src);	/* must outlive the call */
		lv_image_set_src(img, l->src);
	}
	/* Fit the box, keep the aspect ratio. */
	sw = box * 256 / h.w;
	sh = box * 256 / h.h;
	lv_image_set_scale(img, sw < sh ? sw : sh);
	return true;
}

void chumby_label_scroll_speed(lv_obj_t *label, const char *text, int px_per_s)
{
	const lv_font_t *f = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	int32_t w = LV_LABEL_WAIT_CHAR_COUNT * lv_font_get_glyph_width(f, ' ', ' ');
	const unsigned char *p;

	for (p = (const unsigned char *)text; *p; p++)
		if (*p < 0x80 || *p >= 0xc0)	/* a character's first byte */
			w += lv_font_get_glyph_width(f, *p < 0x80 ? *p : '?', p[1]);
	lv_obj_set_style_anim_duration(label, px_per_s > 0 ? (uint32_t)w * 1000 / px_per_s : 0,
				       LV_PART_MAIN);
}

/* LVGL's circular scroll with an animation template (2 repeats, a pause
 * first), switched to dots once done. */
static lv_anim_t scroll_twice;

static void scroll_done_async(void *label)
{
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

static void scroll_done(lv_anim_t *a)
{
	/* Not from inside the label's own animation. */
	lv_async_call(scroll_done_async, a->var);
}

void chumby_scroll_label_init(lv_obj_t *l)
{
	if (!scroll_twice.completed_cb) {
		lv_anim_init(&scroll_twice);
		lv_anim_set_repeat_count(&scroll_twice, 2);
		lv_anim_set_delay(&scroll_twice, 1500);		/* read the start first */
		lv_anim_set_completed_cb(&scroll_twice, scroll_done);
	}
	/* One line, always: dots need a fixed height (else it wraps). */
	lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
	lv_obj_set_style_anim(l, &scroll_twice, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_label_set_text(l, "");
}

void chumby_scroll_label_set(lv_obj_t *l, const char *text)
{
	if (!strcmp(lv_label_get_text(l), text))
		return;
	lv_anim_delete(l, NULL);	/* start over, from the beginning */
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	chumby_label_scroll_speed(l, text, CHUMBY_SCROLL_SPEED);
	lv_label_set_text(l, text);
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
	/* On the accent: the background's shade (light by day, black at night). */
	lv_style_set_text_color(&st_btn_checked, chumby_color(CHUMBY_COLOR_BG));
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
					       mode == CHUMBY_MODE_NIGHT, chumby_font(CHUMBY_FONT_BODY));
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
	/* chumby-wm sizes app windows to the area under the tab bar. */
	lv_display_t *disp = chumby_window_create(app_id, CHUMBY_APP_W, CHUMBY_APP_H);

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
		/* The compositor went away (session restart): so do we. */
		if (pfd[0].revents & (POLLHUP | POLLERR)) {
			fprintf(stderr, "libchumby: lost the Wayland compositor\n");
			exit(0);
		}
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

static void write_str(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fputs(s, f);
		fclose(f);
	}
}

/*
 * FB_BLANK_UNBLANK / FB_BLANK_POWERDOWN: pwm-backlight turns the PWM off.
 * Full speed before the light comes on, slow only once it's dark.
 */
void chumby_screen_power(bool on)
{
	if (on)
		write_str(CPU_GOVERNOR, "performance\n");
	write_str(BACKLIGHT "bl_power", on ? "0\n" : "4\n");
	if (!on)
		write_str(CPU_GOVERNOR, "powersave\n");
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

static void awake_path(char *out, size_t size)
{
	snprintf(out, size, "%s/awake/%d", chumby_run_dir(), (int)getpid());
}

static void awake_clear(void)
{
	char path[256];

	awake_path(path, sizeof(path));
	unlink(path);
}

void chumby_stay_awake(bool on)
{
	static bool registered;
	char path[256];
	int fd;

	if (!on) {
		awake_clear();
		return;
	}
	snprintf(path, sizeof(path), "%s/awake", chumby_run_dir());
	mkdir(path, 0755);
	awake_path(path, sizeof(path));
	fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0)
		close(fd);
	if (!registered) {
		atexit(awake_clear);
		registered = true;
	}
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
