// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-shell: the Chumby One's system UI on chumby-wm.
 *
 *   panel (app_id "chumby-panel", left edge, 56 px):
 *     home button, an icon per running app (tap: switch, long press:
 *     close), quick volume and brightness sliders
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
#define PANEL_W		56
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

static lv_obj_t *panel_scr, *home_scr;
static lv_obj_t *btn_home, *slider_vol, *slider_bri, *lbl_vol, *lbl_bri;

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

static lv_obj_t *icon_button(lv_obj_t *parent, const char *sym, bool active)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, 46, 40);
	lv_obj_set_style_radius(b, 8, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	/* Colors come from the libchumby theme; only "active" is local. */
	if (active)
		lv_obj_set_style_bg_color(b, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
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
	lv_obj_set_style_bg_color(btn_home, any_active ?
				  chumby_color(CHUMBY_COLOR_SURFACE) :
				  chumby_color(CHUMBY_COLOR_ACCENT), 0);
}

static void home_btn_cb(lv_event_t *e)
{
	(void)e;
	show_home();
}

static void vol_cb(lv_event_t *e)
{
	(void)e;
	chumby_volume_set(lv_slider_get_value(slider_vol));
}

static void bri_cb(lv_event_t *e)
{
	(void)e;
	chumby_brightness_set(lv_slider_get_value(slider_bri));
}

static lv_obj_t *vslider(lv_obj_t *parent, lv_event_cb_t cb)
{
	lv_obj_t *s = lv_slider_create(parent);

	lv_obj_set_size(s, 10, 72);
	lv_slider_set_range(s, 0, 100);
	lv_obj_set_ext_click_area(s, 8);
	lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
	return s;
}

static void build_panel(lv_obj_t *scr)
{
	lv_obj_t *quick, *col, *l;

	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_all(scr, 4, 0);
	lv_obj_set_style_pad_row(scr, 4, 0);
	lv_obj_set_scrollable(scr, false);

	btn_home = icon_button(scr, LV_SYMBOL_HOME, true);
	lv_obj_add_event_cb(btn_home, home_btn_cb, LV_EVENT_CLICKED, NULL);

	panel_apps = lv_obj_create(scr);
	lv_obj_remove_style_all(panel_apps);
	lv_obj_set_width(panel_apps, LV_PCT(100));
	lv_obj_set_flex_grow(panel_apps, 1);
	lv_obj_set_flex_flow(panel_apps, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel_apps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(panel_apps, 4, 0);
	lv_obj_set_scroll_dir(panel_apps, LV_DIR_VER);

	/* Quick controls: volume | brightness */
	quick = lv_obj_create(scr);
	lv_obj_remove_style_all(quick);
	lv_obj_set_size(quick, LV_PCT(100), 96);
	lv_obj_set_flex_flow(quick, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(quick, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(quick, false);

	col = lv_obj_create(quick);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, 22, 96);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 4, 0);
	slider_vol = vslider(col, vol_cb);
	lbl_vol = l = lv_label_create(col);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
	lv_label_set_text(l, LV_SYMBOL_VOLUME_MAX);

	col = lv_obj_create(quick);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, 22, 96);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 4, 0);
	slider_bri = vslider(col, bri_cb);
	lbl_bri = l = lv_label_create(col);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
	lv_label_set_text(l, LV_SYMBOL_EYE_OPEN);
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

		lv_obj_set_size(tile, 116, 84);
		lv_obj_set_style_radius(tile, 12, 0);
		lv_obj_set_style_shadow_width(tile, 0, 0);
		lv_obj_set_style_bg_color(tile, chumby_color(CHUMBY_COLOR_SURFACE), 0);
		lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, &apps[i]);

		ic = lv_label_create(tile);
		lv_label_set_text(ic, apps[i].icon);
		lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
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
	/* Apply the policy brightness on each switch (and at start); the
	 * slider can override it until the next switch. */
	chumby_brightness_set(mode == CHUMBY_MODE_NIGHT ? p->night_brightness
							 : p->day_brightness);
	if (slider_bri)
		lv_slider_set_value(slider_bri, chumby_brightness_get(), LV_ANIM_OFF);
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
	int wheel = 0, v;

	(void)user;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_REL && ev.code == REL_WHEEL)
			wheel += ev.value;
	if (!wheel)
		return;
	v = lv_slider_get_value(slider_vol) + wheel * 2;
	if (v < 0)
		v = 0;
	if (v > 100)
		v = 100;
	lv_slider_set_value(slider_vol, v, LV_ANIM_OFF);
	chumby_volume_set(v);
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
		lv_slider_set_value(slider_vol, v, LV_ANIM_OFF);
		lv_timer_delete(t);
	}
}

int main(void)
{
	lv_display_t *panel, *home;

	chumby_init();
	load_apps();
	ftl_init();

	home = chumby_window_create("chumby-home", 320 - PANEL_W, 240);
	panel = chumby_window_create("chumby-panel", PANEL_W, 240);
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

	return chumby_run();
}
