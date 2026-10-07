// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-shell: the Chumby One's system UI on chumby-wm.
 *
 *   panel (app_id "chumby-panel", left edge, CHUMBY_PANEL_W):
 *     home button, an icon per running app (tap: switch, long press:
 *     close), volume and brightness buttons
 *   popup (app_id "chumby-popup", over the app area, transient): the
 *     volume or brightness control, opened from the panel or the knob
 *   home (app_id "chumby-home", behind all apps): the list of installed
 *     apps, one ".app" descriptor each in /usr/share/chumby/apps
 *
 * Running apps are tracked and switched with wlr-foreign-toplevel-
 * management on a second Wayland connection. The knob changes the volume
 * from anywhere; the backlight follows the day/night policy.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

#include <chumby.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#define APPS_DIR	"/usr/share/chumby/apps"
#define MAX_APPS	16

/* ------------------------------------------------------------------ */
/* Installed apps                                                     */

struct app_info {
	char id[32];		/* xdg app_id the app uses (= its window title) */
	char name[32];
	char exec[128];
	const char *icon;	/* LV_SYMBOL_* */
};

static struct app_info apps[MAX_APPS];
static int n_apps;

static const struct { const char *name, *sym; } icons[] = {
	{ "audio", LV_SYMBOL_AUDIO }, { "bell", LV_SYMBOL_BELL },
	{ "home", LV_SYMBOL_HOME }, { "settings", LV_SYMBOL_SETTINGS },
	{ "image", LV_SYMBOL_IMAGE }, { "list", LV_SYMBOL_LIST },
	{ "wifi", LV_SYMBOL_WIFI }, { "gps", LV_SYMBOL_GPS },
	{ "video", LV_SYMBOL_VIDEO }, { "envelope", LV_SYMBOL_ENVELOPE },
	{ "eye", LV_SYMBOL_EYE_OPEN }, { "file", LV_SYMBOL_FILE },
	{ "charge", LV_SYMBOL_CHARGE }, { "play", LV_SYMBOL_PLAY },
	{ "call", LV_SYMBOL_CALL }, { "keyboard", LV_SYMBOL_KEYBOARD },
};

static const char *icon_symbol(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(icons) / sizeof(icons[0]); i++)
		if (!strcmp(icons[i].name, name))
			return icons[i].sym;
	return LV_SYMBOL_FILE;
}

/* .app files: "key = value" lines with Id, Name, Exec, Icon. */
static void load_apps(void)
{
	DIR *d = opendir(APPS_DIR);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d)) && n_apps < MAX_APPS) {
		char path[300], line[256], key[32], val[200];
		struct app_info *a = &apps[n_apps];
		size_t len = strlen(e->d_name);
		FILE *f;

		if (len < 5 || strcmp(e->d_name + len - 4, ".app"))
			continue;
		snprintf(path, sizeof(path), APPS_DIR "/%s", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		memset(a, 0, sizeof(*a));
		a->icon = LV_SYMBOL_FILE;
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, " %31[^= ] = %199[^\n]", key, val) != 2)
				continue;
			if (!strcmp(key, "Id"))
				snprintf(a->id, sizeof(a->id), "%.31s", val);
			else if (!strcmp(key, "Name"))
				snprintf(a->name, sizeof(a->name), "%.31s", val);
			else if (!strcmp(key, "Exec"))
				snprintf(a->exec, sizeof(a->exec), "%.127s", val);
			else if (!strcmp(key, "Icon"))
				a->icon = icon_symbol(val);
		}
		fclose(f);
		if (a->id[0] && a->exec[0]) {
			if (!a->name[0])
				snprintf(a->name, sizeof(a->name), "%s", a->id);
			n_apps++;
		}
	}
	closedir(d);
}

static struct app_info *app_by_id(const char *id)
{
	int i;

	for (i = 0; i < n_apps; i++)
		if (!strcmp(apps[i].id, id))
			return &apps[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Running apps (wlr-foreign-toplevel-management)                     */

struct toplevel {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char app_id[32];
	bool activated, minimized;
	lv_obj_t *button;	/* in the panel */
	struct toplevel *next;
};

static struct wl_display *wl;
static struct wl_seat *seat;
static struct zwlr_foreign_toplevel_manager_v1 *ftl_manager;
static struct toplevel *toplevels;

static lv_obj_t *panel_apps;	/* column of running-app buttons */

static void panel_rebuild(void);

static void tl_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t)
{
	(void)d; (void)h; (void)t;
}

static void tl_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id)
{
	struct toplevel *t = d;

	(void)h;
	snprintf(t->app_id, sizeof(t->app_id), "%s", id);
}

static void tl_output(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
	(void)d; (void)h; (void)o;
}

static void tl_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st)
{
	struct toplevel *t = d;
	uint32_t *s;

	(void)h;
	t->activated = t->minimized = false;
	wl_array_for_each(s, st) {
		if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
			t->activated = true;
		else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
			t->minimized = true;
	}
}

static void tl_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	(void)d; (void)h;
	panel_rebuild();
}

static void tl_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	struct toplevel *t = d, **pp;

	for (pp = &toplevels; *pp; pp = &(*pp)->next) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	}
	zwlr_foreign_toplevel_handle_v1_destroy(h);
	free(t);
	panel_rebuild();
}

static void tl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
		      struct zwlr_foreign_toplevel_handle_v1 *p)
{
	(void)d; (void)h; (void)p;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
	.title = tl_title,
	.app_id = tl_app_id,
	.output_enter = tl_output,
	.output_leave = tl_output,
	.state = tl_state,
	.done = tl_done,
	.closed = tl_closed,
	.parent = tl_parent,
};

static void mgr_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m,
			 struct zwlr_foreign_toplevel_handle_v1 *h)
{
	struct toplevel *t = calloc(1, sizeof(*t));

	(void)d; (void)m;
	if (!t)
		return;
	t->handle = h;
	t->next = toplevels;
	toplevels = t;
	zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, t);
}

static void mgr_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m)
{
	(void)d; (void)m;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener mgr_listener = {
	.toplevel = mgr_toplevel,
	.finished = mgr_finished,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		       const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		ftl_manager = wl_registry_bind(r, name,
			&zwlr_foreign_toplevel_manager_v1_interface, ver < 3 ? ver : 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(ftl_manager, &mgr_listener, NULL);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	}
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r; (void)name;
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

static void wl_readable(int fd, void *user)
{
	(void)fd; (void)user;
	if (wl_display_dispatch(wl) < 0) {
		fprintf(stderr, "chumby-shell: lost the compositor\n");
		exit(1);
	}
}

static void ftl_init(void)
{
	wl = wl_display_connect(NULL);
	if (!wl) {
		fprintf(stderr, "chumby-shell: no Wayland display\n");
		exit(1);
	}
	wl_registry_add_listener(wl_display_get_registry(wl), &reg_listener, NULL);
	wl_display_roundtrip(wl);
	wl_display_roundtrip(wl);
	if (!ftl_manager)
		fprintf(stderr, "chumby-shell: compositor lacks foreign-toplevel-management\n");
	chumby_watch_fd(wl_display_get_fd(wl), wl_readable, NULL);
}

static struct toplevel *running(const char *app_id)
{
	struct toplevel *t;

	for (t = toplevels; t; t = t->next)
		if (!strcmp(t->app_id, app_id))
			return t;
	return NULL;
}

static void activate(struct toplevel *t)
{
	if (seat)
		zwlr_foreign_toplevel_handle_v1_activate(t->handle, seat);
	wl_display_flush(wl);
}

static void show_home(void)
{
	struct toplevel *t;

	for (t = toplevels; t; t = t->next)
		zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
	wl_display_flush(wl);
}

static void launch(struct app_info *a)
{
	struct toplevel *t = running(a->id);
	char *argv[] = { a->exec, NULL };

	if (t)
		activate(t);
	else if (chumby_spawn(a->exec, argv) < 0)
		fprintf(stderr, "chumby-shell: cannot start %s\n", a->exec);
}

/* ------------------------------------------------------------------ */
/* Panel                                                              */

/*
 * Everything here is sized for a resistive touchscreen: panel buttons are
 * the panel's full usable width and at least CHUMBY_TOUCH_MIN tall, and
 * values are changed in a big popup (tap-to-set bar, - / + buttons)
 * rather than with small sliders.
 */
#define BTN_W		(CHUMBY_PANEL_W - 8)
#define BTN_H		CHUMBY_TOUCH_MIN
#define POPUP_IDLE_MS	6000

static lv_obj_t *panel_scr, *home_scr;
static lv_obj_t *btn_home, *btn_vol, *btn_bri;
static int volume_pct = 50;	/* cached: the mixer may not exist yet */

enum popup_kind { POPUP_NONE, POPUP_VOLUME, POPUP_BRIGHTNESS };
static enum popup_kind popup_kind;
static lv_display_t *popup_disp;
static lv_obj_t *popup_bar, *popup_value;
static lv_timer_t *popup_timer;

static void app_btn_cb(lv_event_t *e)
{
	struct toplevel *t = lv_event_get_user_data(e), *i;

	/* The toplevel may have closed since the button was built. */
	for (i = toplevels; i && i != t; i = i->next)
		;
	if (!i)
		return;
	if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
		zwlr_foreign_toplevel_handle_v1_close(t->handle);
	else
		activate(t);
	wl_display_flush(wl);
}

/* Colors come from the libchumby theme; "active" is the only local style. */
static void set_active(lv_obj_t *b, bool active)
{
	if (active)
		lv_obj_set_style_bg_color(b, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	else
		lv_obj_remove_local_style_prop(b, LV_STYLE_BG_COLOR, 0);
}

static lv_obj_t *icon_button(lv_obj_t *parent, const char *sym, bool active)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, BTN_W, BTN_H);
	lv_obj_set_style_radius(b, 10, 0);
	set_active(b, active);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_label_set_text(l, sym);
	lv_obj_center(l);
	return b;
}

static void panel_rebuild(void)
{
	struct toplevel *t;
	bool any_active = false;

	if (!panel_apps)
		return;
	lv_obj_clean(panel_apps);
	for (t = toplevels; t; t = t->next) {
		struct app_info *a = app_by_id(t->app_id);
		bool active = t->activated && !t->minimized;
		lv_obj_t *b = icon_button(panel_apps, a ? a->icon : LV_SYMBOL_FILE, active);

		any_active |= active;
		lv_obj_add_event_cb(b, app_btn_cb, LV_EVENT_SHORT_CLICKED, t);
		lv_obj_add_event_cb(b, app_btn_cb, LV_EVENT_LONG_PRESSED, t);
	}
	/* Home is "active" when no app is in front. */
	set_active(btn_home, !any_active);
}

static void home_btn_cb(lv_event_t *e)
{
	(void)e;
	show_home();
}

/* ------------------------------------------------------------------ */
/* Volume / brightness popup ("chumby-popup", over the app area)      */

static void quick_buttons_update(void)
{
	set_active(btn_vol, popup_kind == POPUP_VOLUME);
	set_active(btn_bri, popup_kind == POPUP_BRIGHTNESS);
}

static int popup_get(void)
{
	return popup_kind == POPUP_VOLUME ? volume_pct : chumby_brightness_get();
}

/* Keep the popup up while it's being used; it closes after POPUP_IDLE_MS. */
static void popup_keepalive(void)
{
	lv_timer_reset(popup_timer);
	lv_timer_resume(popup_timer);
}

static void popup_set(int v)
{
	int min = popup_kind == POPUP_BRIGHTNESS ? 5 : 0;

	if (v < min)
		v = min;
	if (v > 100)
		v = 100;
	if (popup_kind == POPUP_VOLUME) {
		volume_pct = v;
		chumby_volume_set(v);
	} else {
		chumby_brightness_set(v);
	}
	if (popup_bar) {
		lv_slider_set_value(popup_bar, v, LV_ANIM_OFF);
		lv_label_set_text_fmt(popup_value, "%d%%", v);
	}
	popup_keepalive();
}

static void popup_close(void)
{
	if (!popup_disp)
		return;
	chumby_window_close(popup_disp);
	popup_disp = NULL;
	popup_bar = popup_value = NULL;
	popup_kind = POPUP_NONE;
	lv_timer_pause(popup_timer);
	quick_buttons_update();
}

static void popup_timer_cb(lv_timer_t *t)
{
	(void)t;
	popup_close();
}

static void popup_bar_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
		popup_set(lv_slider_get_value(popup_bar));
	else
		popup_keepalive();	/* pressing/dragging */
}

/* - / + : 10 % steps, snapping to multiples of 10. */
static void popup_step_cb(lv_event_t *e)
{
	int dir = (int)(intptr_t)lv_event_get_user_data(e);
	int v = popup_get();

	popup_set(dir > 0 ? (v / 10 + 1) * 10 : ((v + 9) / 10 - 1) * 10);
}

static void popup_bg_cb(lv_event_t *e)
{
	(void)e;
	popup_close();
}

static lv_obj_t *popup_button(lv_obj_t *parent, const char *sym, int dir)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, 108, 60);
	lv_obj_set_style_radius(b, 12, 0);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_label_set_text(l, sym);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, popup_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)dir);
	lv_obj_add_event_cb(b, popup_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)dir);
	return b;
}

static void popup_open(enum popup_kind kind)
{
	bool vol = kind == POPUP_VOLUME;
	lv_obj_t *scr, *icon, *title;

	if (popup_kind == kind && popup_disp) {
		popup_keepalive();
		return;
	}
	if (!popup_disp) {
		popup_disp = chumby_window_create("chumby-popup", CHUMBY_APP_W, CHUMBY_SCREEN_H);
		if (!popup_disp)
			return;
	}
	popup_kind = kind;
	scr = lv_display_get_screen_active(popup_disp);
	lv_obj_clean(scr);
	lv_obj_set_scrollable(scr, false);
	/* Tapping empty space closes; widgets don't bubble their clicks up. */
	lv_obj_add_event_cb(scr, popup_bg_cb, LV_EVENT_CLICKED, NULL);

	icon = lv_label_create(scr);
	lv_label_set_text(icon, vol ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_EYE_OPEN);
	lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(icon, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 16, 20);

	title = lv_label_create(scr);
	lv_label_set_text(title, vol ? "Volume" : "Brightness");
	lv_obj_set_style_text_font(title, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 56, 18);

	popup_value = lv_label_create(scr);
	lv_obj_set_style_text_font(popup_value, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_style_text_color(popup_value, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_align(popup_value, LV_ALIGN_TOP_RIGHT, -16, 18);

	/* A thick bar: tapping anywhere on it sets the value, no knob to aim at. */
	popup_bar = lv_slider_create(scr);
	lv_obj_set_size(popup_bar, CHUMBY_APP_W - 32, 52);
	lv_obj_align(popup_bar, LV_ALIGN_TOP_MID, 0, 80);
	lv_slider_set_range(popup_bar, vol ? 0 : 5, 100);
	lv_obj_set_style_radius(popup_bar, 12, LV_PART_MAIN);
	lv_obj_set_style_radius(popup_bar, 12, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(popup_bar, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_pad_all(popup_bar, 0, LV_PART_KNOB);
	lv_obj_set_ext_click_area(popup_bar, 12);
	lv_obj_add_event_cb(popup_bar, popup_bar_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(popup_bar, popup_bar_cb, LV_EVENT_PRESSING, NULL);

	lv_obj_align(popup_button(scr, LV_SYMBOL_MINUS, -1), LV_ALIGN_BOTTOM_LEFT, 16, -20);
	lv_obj_align(popup_button(scr, LV_SYMBOL_PLUS, 1), LV_ALIGN_BOTTOM_RIGHT, -16, -20);

	popup_set(popup_get());
	quick_buttons_update();
}

/* Panel button: open its popup, or close it if it's already showing. */
static void quick_btn_cb(lv_event_t *e)
{
	enum popup_kind kind = (enum popup_kind)(intptr_t)lv_event_get_user_data(e);

	if (popup_kind == kind)
		popup_close();
	else
		popup_open(kind);
}

static void build_panel(lv_obj_t *scr)
{
	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_all(scr, 4, 0);
	lv_obj_set_style_pad_row(scr, 4, 0);
	lv_obj_set_scrollable(scr, false);

	btn_home = icon_button(scr, LV_SYMBOL_HOME, true);
	lv_obj_add_event_cb(btn_home, home_btn_cb, LV_EVENT_CLICKED, NULL);

	/* Running apps: two fit; more scroll. */
	panel_apps = lv_obj_create(scr);
	lv_obj_remove_style_all(panel_apps);
	lv_obj_set_width(panel_apps, LV_PCT(100));
	lv_obj_set_flex_grow(panel_apps, 1);
	lv_obj_set_flex_flow(panel_apps, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel_apps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(panel_apps, 4, 0);
	lv_obj_set_scroll_dir(panel_apps, LV_DIR_VER);

	btn_vol = icon_button(scr, LV_SYMBOL_VOLUME_MAX, false);
	lv_obj_add_event_cb(btn_vol, quick_btn_cb, LV_EVENT_CLICKED,
			    (void *)(intptr_t)POPUP_VOLUME);
	btn_bri = icon_button(scr, LV_SYMBOL_EYE_OPEN, false);
	lv_obj_add_event_cb(btn_bri, quick_btn_cb, LV_EVENT_CLICKED,
			    (void *)(intptr_t)POPUP_BRIGHTNESS);

	popup_timer = lv_timer_create(popup_timer_cb, POPUP_IDLE_MS, NULL);
	lv_timer_pause(popup_timer);
}

/* ------------------------------------------------------------------ */
/* Home: installed apps                                               */

static lv_obj_t *home_title;

static void tile_cb(lv_event_t *e)
{
	launch(lv_event_get_user_data(e));
}

static void build_home(lv_obj_t *scr)
{
	lv_obj_t *grid;
	int i;

	lv_obj_set_scrollable(scr, false);
	home_title = lv_label_create(scr);
	lv_label_set_text(home_title, "Apps");
	lv_obj_set_style_text_font(home_title, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_align(home_title, LV_ALIGN_TOP_LEFT, 12, 6);

	grid = lv_obj_create(scr);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, LV_PCT(100), 196);
	lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_all(grid, 8, 0);
	lv_obj_set_style_pad_gap(grid, 8, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	for (i = 0; i < n_apps; i++) {
		lv_obj_t *tile = lv_button_create(grid), *ic, *nm;

		/* Two big tiles per row. */
		lv_obj_set_size(tile, (CHUMBY_APP_W - 3 * 8) / 2, 84);
		lv_obj_set_style_radius(tile, 12, 0);
		lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, &apps[i]);

		ic = lv_label_create(tile);
		lv_label_set_text(ic, apps[i].icon);
		lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, 0);
		lv_obj_set_style_text_color(ic, chumby_color(CHUMBY_COLOR_ACCENT), 0);
		lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 6);

		nm = lv_label_create(tile);
		lv_label_set_text(nm, apps[i].name);
		lv_obj_set_style_text_color(nm, chumby_color(CHUMBY_COLOR_TEXT), 0);
		lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -4);
	}
	if (!n_apps) {
		lv_obj_t *l = lv_label_create(grid);

		lv_label_set_text(l, "No apps installed in " APPS_DIR);
	}
}

/* Rebuild the styled parts when day/night flips (colors are per mode). */
static void on_mode(chumby_mode_t mode, void *user)
{
	const struct chumby_policy *p = chumby_policy();

	(void)user;
	if (home_scr) {
		lv_obj_clean(home_scr);
		build_home(home_scr);
	}
	panel_rebuild();
	quick_buttons_update();
	/* Apply the policy brightness on each switch (and at start); the
	 * brightness popup can override it until the next switch. */
	chumby_brightness_set(mode == CHUMBY_MODE_NIGHT ? p->night_brightness
							 : p->day_brightness);
	/* Rebuild an open popup in the new colors. */
	if (popup_disp) {
		enum popup_kind kind = popup_kind;

		popup_kind = POPUP_NONE;
		popup_open(kind);
	}
}

/* ------------------------------------------------------------------ */
/* Knob                                                               */

static int find_input(const char *name)
{
	char dev[32], devname[64];
	int i, fd;

	for (i = 0; i < 16; i++) {
		snprintf(dev, sizeof(dev), "/dev/input/event%d", i);
		fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(devname)), devname) >= 0 &&
		    !strcmp(devname, name))
			return fd;
		close(fd);
	}
	return -1;
}

static void knob_readable(int fd, void *user)
{
	struct input_event ev;
	int wheel = 0;

	(void)user;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_REL && ev.code == REL_WHEEL)
			wheel += ev.value;
	if (!wheel)
		return;
	/* Show the volume popup as feedback while the knob turns. */
	popup_open(POPUP_VOLUME);
	popup_set(volume_pct + wheel * 2);
}

/*
 * Debug/automation: SIGUSR1 opens the volume popup, SIGUSR2 brightness
 * (e.g. `killall -USR1 chumby-shell` over ssh, or to screenshot it).
 */
static volatile sig_atomic_t popup_request;

static void popup_signal(int sig)
{
	popup_request = sig == SIGUSR1 ? POPUP_VOLUME : POPUP_BRIGHTNESS;
}

static void popup_request_cb(lv_timer_t *t)
{
	(void)t;
	if (popup_request) {
		popup_open((enum popup_kind)popup_request);
		popup_request = 0;
	}
}

/* Reap apps we launched once they exit. */
static void reaper_cb(lv_timer_t *t)
{
	(void)t;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

/* The sound card is a module and may appear after we start. */
static void volume_sync_cb(lv_timer_t *t)
{
	int v = chumby_volume_get();

	if (v >= 0) {
		volume_pct = v;
		lv_timer_delete(t);
	}
}

int main(void)
{
	lv_display_t *panel, *home;

	chumby_init();
	load_apps();
	ftl_init();

	home = chumby_window_create("chumby-home", CHUMBY_APP_W, CHUMBY_SCREEN_H);
	panel = chumby_window_create("chumby-panel", CHUMBY_PANEL_W, CHUMBY_SCREEN_H);
	if (!home || !panel) {
		fprintf(stderr, "chumby-shell: cannot create windows\n");
		return 1;
	}
	home_scr = lv_display_get_screen_active(home);
	panel_scr = lv_display_get_screen_active(panel);

	build_panel(panel_scr);
	chumby_on_mode_change(on_mode, NULL);	/* builds home, sets brightness */

	chumby_watch_fd(find_input("rotary"), knob_readable, NULL);
	lv_timer_ready(lv_timer_create(volume_sync_cb, 2000, NULL));
	lv_timer_create(reaper_cb, 1000, NULL);
	signal(SIGUSR1, popup_signal);
	signal(SIGUSR2, popup_signal);
	lv_timer_create(popup_request_cb, 100, NULL);

	return chumby_run();
}
