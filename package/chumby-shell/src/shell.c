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
 * from anywhere; the backlight follows the day/night policy and is off
 * altogether in the policy's screen-off hours.
 *
 * The snooze button (top of the case) brings up the clock from anywhere,
 * lights the screen for a few seconds at night, and snoozes a ringing
 * alarm. Alarms are rung here rather than in the clock app, so they go
 * off whatever is open; the clock app edits them and shows the ringing
 * state, talking to us through the run dir (see chumby.h).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include <chumby.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#define APPS_DIR	"/usr/share/chumby/apps"
#define MAX_APPS	16
#define CLOCK_ID	"Clock"		/* app the snooze button brings up */
#define ALARM_TONE	"/usr/bin/chumby-alarm-tone"
#define RADIO		"/usr/bin/radio"
#define RADIO_PIDFILE	"/var/run/radio-loop.pid"
#define RING_MAX_S	(10 * 60)	/* give up ringing after this */

/* ------------------------------------------------------------------ */
/* Installed apps                                                     */

struct app_info {
	char id[32];		/* xdg app_id the app uses (= its window title) */
	char name[32];
	char exec[128];
	const char *icon;	/* LV_SYMBOL_* */
	int starting_pid;	/* launched, no window yet (0 = none) */
	time_t starting_since;
};

/* An app takes seconds to map its window on this CPU: don't start it again
 * meanwhile (every tap or snooze press would, filling the 64 MB). */
#define LAUNCH_TIMEOUT_S	20

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
	struct toplevel *t = d;
	struct app_info *a = app_by_id(t->app_id);

	(void)h;
	if (a)
		a->starting_pid = 0;	/* its window is up */
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
	int pid;

	if (t) {
		activate(t);
		return;
	}
	if (a->starting_pid && time(NULL) - a->starting_since < LAUNCH_TIMEOUT_S)
		return;		/* already on its way */
	pid = chumby_spawn(a->exec, argv);
	if (pid < 0) {
		fprintf(stderr, "chumby-shell: cannot start %s\n", a->exec);
		return;
	}
	a->starting_pid = pid;
	a->starting_since = time(NULL);
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
#define PANEL_DIM_OPA	LV_OPA_30	/* at rest: barely there */
#define PANEL_IDLE_MS	4000
#define PANEL_FADE_IN_MS	80
#define PANEL_FADE_OUT_MS	600

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

/*
 * The panel stays out of the way: no button plates and icons at
 * PANEL_DIM_OPA at rest. Touching it brings it to full contrast, and it
 * fades back PANEL_IDLE_MS after the last touch (once any popup closes).
 */
static lv_opa_t panel_opa = LV_OPA_COVER;
static lv_timer_t *panel_timer;

static void panel_opa_cb(void *var, int32_t v)
{
	uint32_t i;

	(void)var;
	panel_opa = v;
	for (i = 0; i < lv_obj_get_child_count(panel_scr); i++)
		lv_obj_set_style_opa(lv_obj_get_child(panel_scr, i), v, 0);
}

static void panel_fade(lv_opa_t to, uint32_t ms)
{
	lv_anim_t a;

	lv_anim_delete(panel_scr, panel_opa_cb);
	if (to == panel_opa)
		return;
	lv_anim_init(&a);
	lv_anim_set_var(&a, panel_scr);
	lv_anim_set_exec_cb(&a, panel_opa_cb);
	lv_anim_set_values(&a, panel_opa, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_start(&a);
}

static void panel_wake(void)
{
	panel_fade(LV_OPA_COVER, PANEL_FADE_IN_MS);
	lv_timer_reset(panel_timer);
	lv_timer_resume(panel_timer);
}

static void panel_timer_cb(lv_timer_t *t)
{
	if (popup_disp)
		return;		/* stay lit while a popup is up */
	panel_fade(PANEL_DIM_OPA, PANEL_FADE_OUT_MS);
	lv_timer_pause(t);
}

/* Presses anywhere in the panel bubble up to its screen. */
static void panel_press_cb(lv_event_t *e)
{
	(void)e;
	panel_wake();
}

/* The open popup's button: its icon in the accent color. */
static void set_active(lv_obj_t *b, bool active)
{
	if (active)
		lv_obj_set_style_text_color(b, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	else
		lv_obj_remove_local_style_prop(b, LV_STYLE_TEXT_COLOR, 0);
}

static lv_obj_t *icon_button(lv_obj_t *parent, const char *sym)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, BTN_W, BTN_H);
	lv_obj_set_style_radius(b, 10, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	/* A plate only while pressed. */
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_event_bubble(b, true);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_label_set_text(l, sym);
	lv_obj_center(l);
	return b;
}

/* The app in front gets a dot at its left, like the macOS dock. */
static void running_dot(lv_obj_t *b)
{
	lv_obj_t *dot = lv_obj_create(b);

	lv_obj_remove_style_all(dot);
	lv_obj_set_size(dot, 6, 6);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(dot, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_clickable(dot, false);
	lv_obj_align(dot, LV_ALIGN_LEFT_MID, 1, 0);
}

static void panel_rebuild(void)
{
	struct toplevel *t;

	if (!panel_apps)
		return;
	lv_obj_clean(panel_apps);
	for (t = toplevels; t; t = t->next) {
		struct app_info *a = app_by_id(t->app_id);
		lv_obj_t *b = icon_button(panel_apps, a ? a->icon : LV_SYMBOL_FILE);

		if (t->activated && !t->minimized)
			running_dot(b);
		lv_obj_add_event_cb(b, app_btn_cb, LV_EVENT_SHORT_CLICKED, t);
		lv_obj_add_event_cb(b, app_btn_cb, LV_EVENT_LONG_PRESSED, t);
	}
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
	lv_obj_t *scr, *icon, *title, *minus;

	panel_wake();
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
	lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 16, 14);

	/* Header: title beside the icon, value at the right, all on the
	 * icon's centre line. */
	title = lv_label_create(scr);
	lv_label_set_text(title, vol ? "Volume" : "Brightness");
	lv_obj_set_style_text_font(title, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_align_to(title, icon, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

	popup_value = lv_label_create(scr);
	lv_obj_set_style_text_font(popup_value, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_style_text_color(popup_value, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_align(popup_value, LV_ALIGN_TOP_RIGHT, -16, 0);
	chumby_center_y_on(popup_value, icon);

	/* A thick bar: tapping anywhere on it sets the value, no knob to aim at. */
	popup_bar = lv_slider_create(scr);
	lv_obj_set_size(popup_bar, CHUMBY_APP_W - 32, 52);
	lv_obj_align(popup_bar, LV_ALIGN_TOP_MID, 0, 0);
	lv_slider_set_range(popup_bar, vol ? 0 : 5, 100);
	lv_obj_set_style_radius(popup_bar, 12, LV_PART_MAIN);
	lv_obj_set_style_radius(popup_bar, 12, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(popup_bar, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_pad_all(popup_bar, 0, LV_PART_KNOB);
	lv_obj_set_ext_click_area(popup_bar, 12);
	lv_obj_add_event_cb(popup_bar, popup_bar_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(popup_bar, popup_bar_cb, LV_EVENT_PRESSING, NULL);

	minus = popup_button(scr, LV_SYMBOL_MINUS, -1);
	lv_obj_align(minus, LV_ALIGN_BOTTOM_LEFT, 16, -20);
	lv_obj_align(popup_button(scr, LV_SYMBOL_PLUS, 1), LV_ALIGN_BOTTOM_RIGHT, -16, -20);
	/* The bar sits midway between the header and the buttons. */
	chumby_center_y_between(popup_bar, icon, minus);

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

	btn_home = icon_button(scr, LV_SYMBOL_HOME);
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
	lv_obj_set_event_bubble(panel_apps, true);

	btn_vol = icon_button(scr, LV_SYMBOL_VOLUME_MAX);
	lv_obj_add_event_cb(btn_vol, quick_btn_cb, LV_EVENT_CLICKED,
			    (void *)(intptr_t)POPUP_VOLUME);
	btn_bri = icon_button(scr, LV_SYMBOL_EYE_OPEN);
	lv_obj_add_event_cb(btn_bri, quick_btn_cb, LV_EVENT_CLICKED,
			    (void *)(intptr_t)POPUP_BRIGHTNESS);

	popup_timer = lv_timer_create(popup_timer_cb, POPUP_IDLE_MS, NULL);
	lv_timer_pause(popup_timer);

	lv_obj_add_event_cb(scr, panel_press_cb, LV_EVENT_PRESSED, NULL);
	panel_timer = lv_timer_create(panel_timer_cb, PANEL_IDLE_MS, NULL);
	lv_timer_pause(panel_timer);
	panel_opa_cb(NULL, PANEL_DIM_OPA);
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
	lv_obj_align(home_title, LV_ALIGN_TOP_LEFT, 12, 0);
	/* Centred in the band above the first row of tiles (grid top 44 +
	 * its 8 px padding). */
	chumby_center_y_at(home_title, (CHUMBY_SCREEN_H - 196 + 8) / 2);

	grid = lv_obj_create(scr);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, LV_PCT(100), 196);
	lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_all(grid, 8, 0);
	lv_obj_set_style_pad_gap(grid, 8, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	for (i = 0; i < n_apps; i++) {
		lv_obj_t *tile = lv_button_create(grid), *pair, *ic, *nm;

		/* Two big tiles per row. */
		lv_obj_set_size(tile, (CHUMBY_APP_W - 3 * 8) / 2, 84);
		lv_obj_set_style_radius(tile, 12, 0);
		lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, &apps[i]);
		/* Icon over name, the pair centred in the tile. */
		pair = chumby_group(tile, LV_FLEX_FLOW_COLUMN, 8);
		lv_obj_align(pair, LV_ALIGN_TOP_MID, 0, 0);

		ic = lv_label_create(pair);
		lv_label_set_text(ic, apps[i].icon);
		lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, 0);
		lv_obj_set_style_text_color(ic, chumby_color(CHUMBY_COLOR_ACCENT), 0);

		nm = lv_label_create(pair);
		lv_label_set_text(nm, apps[i].name);
		lv_obj_set_style_text_color(nm, chumby_color(CHUMBY_COLOR_TEXT), 0);
		chumby_center_y_on(pair, tile);
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

static void screen_wake(void);

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
	screen_wake();
	/* Show the volume popup as feedback while the knob turns. */
	popup_open(POPUP_VOLUME);
	popup_set(volume_pct + wheel * 2);
}

/* ------------------------------------------------------------------ */
/* Screen power                                                       */

static uint64_t wake_until;	/* monotonic ms: lit until then at night */
static int screen_applied = -1;	/* what we last wrote, -1 = nothing yet */

/* The snooze button turns the screen on or off for good, overriding the
 * schedule until it next changes (21:00 / 07:00 by default). */
enum { OVERRIDE_NONE, OVERRIDE_ON, OVERRIDE_OFF };
static int screen_override;
static int policy_on_last = -1;

static chumby_alarm_state_t alarm_state;

static uint64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

/*
 * On outside the screen-off hours (and always while the clock isn't set:
 * we can't tell if it's night), unless the snooze button turned it off;
 * on at night when the snooze button turned it on or the knob woke it;
 * always while an alarm rings.
 */
static void screen_update(void)
{
	const struct chumby_policy *p = chumby_policy();
	bool policy_on = !chumby_time_valid() ||
			 p->screen_off_start == p->screen_off_end ||
			 !chumby_in_window(p->screen_off_start, p->screen_off_end);
	bool on;

	if (policy_on != policy_on_last) {	/* the schedule moved on */
		policy_on_last = policy_on;
		screen_override = OVERRIDE_NONE;
	}
	if (alarm_state == CHUMBY_ALARM_RINGING || mono_ms() < wake_until)
		on = true;	/* ringing, or the knob woke it briefly */
	else if (screen_override != OVERRIDE_NONE)
		on = screen_override == OVERRIDE_ON;
	else
		on = policy_on;

	if (on == screen_applied)
		return;
	screen_applied = on;
	chumby_screen_power(on);
	if (!on)
		popup_close();
}

static void screen_wake(void)
{
	wake_until = mono_ms() + chumby_policy()->wake_seconds * 1000ull;
	screen_update();
}

/* Touches keep a woken screen on (they don't wake a dark one: chumby-wm
 * drops those, so nothing gets pressed blind). */
static void touch_readable(int fd, void *user)
{
	struct input_event ev;
	bool any = false;

	(void)user;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev))
		any = true;
	if (any && mono_ms() < wake_until)
		screen_wake();
}

/* ------------------------------------------------------------------ */
/* Alarms                                                             */

static struct chumby_alarm alarms[CHUMBY_MAX_ALARMS];
static int n_alarms;
static struct timespec alarms_mtime;
static bool alarms_stat_ok;
static time_t ring_started, snooze_until;
static long last_minute = -1;
static int tone_pid = -1, volume_before = -1;

static void alarms_reload(void)
{
	struct stat st;
	bool ok = stat(chumby_alarms_path(), &st) == 0;

	if (ok == alarms_stat_ok && (!ok ||
	    (st.st_mtim.tv_sec == alarms_mtime.tv_sec &&
	     st.st_mtim.tv_nsec == alarms_mtime.tv_nsec)))
		return;
	alarms_stat_ok = ok;
	if (ok)
		alarms_mtime = st.st_mtim;
	n_alarms = chumby_alarms_load(alarms, CHUMBY_MAX_ALARMS);
}

/* For the clock app: "ringing", "snoozed <epoch>", or no file. */
static void alarm_publish(void)
{
	char path[240], tmp[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/alarm", chumby_run_dir());
	if (alarm_state == CHUMBY_ALARM_IDLE) {
		unlink(path);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	if (alarm_state == CHUMBY_ALARM_RINGING)
		fputs("ringing\n", f);
	else
		fprintf(f, "snoozed %lld\n", (long long)snooze_until);
	fclose(f);
	rename(tmp, path);
}

static bool radio_playing(void)
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

static void tone_stop(void)
{
	if (tone_pid > 0)
		kill(tone_pid, SIGTERM);
	tone_pid = -1;
	if (volume_before >= 0) {
		volume_pct = volume_before;
		chumby_volume_set(volume_before);
		volume_before = -1;
	}
}

static void show_clock(void)
{
	struct app_info *clock = app_by_id(CLOCK_ID);

	popup_close();
	if (clock)
		launch(clock);
}

static void alarm_ring(void)
{
	const struct chumby_policy *p = chumby_policy();
	char *tone_argv[] = { "chumby-alarm-tone", NULL };
	char *radio_argv[] = { "radio", "off", NULL };
	int v;

	if (alarm_state == CHUMBY_ALARM_RINGING)
		return;
	alarm_state = CHUMBY_ALARM_RINGING;
	ring_started = time(NULL);
	snooze_until = 0;
	/* The radio holds the sound card; the tone retries until it's free. */
	if (radio_playing())
		chumby_spawn(RADIO, radio_argv);
	v = chumby_volume_get();
	if (v >= 0 && v < p->alarm_volume) {
		volume_before = v;
		volume_pct = p->alarm_volume;
		chumby_volume_set(p->alarm_volume);
	}
	tone_pid = chumby_spawn(ALARM_TONE, tone_argv);
	alarm_publish();
	show_clock();
	screen_update();
}

static void alarm_snooze(void)
{
	if (alarm_state != CHUMBY_ALARM_RINGING)
		return;
	tone_stop();
	alarm_state = CHUMBY_ALARM_SNOOZED;
	snooze_until = time(NULL) + chumby_policy()->snooze_minutes * 60;
	alarm_publish();
}

static void alarm_stop(void)
{
	tone_stop();
	alarm_state = CHUMBY_ALARM_IDLE;
	snooze_until = 0;
	alarm_publish();
	/* You're up: keep the screen on (until the schedule next changes). */
	screen_override = OVERRIDE_ON;
	screen_update();
}

/* Once a second: due alarms, snooze expiry, ring timeout, screen hours. */
static void alarm_tick_cb(lv_timer_t *t)
{
	time_t now = time(NULL);
	struct tm tm;
	long minute = (long)(now / 60);
	bool due = false, changed = false;
	int i;

	(void)t;
	alarms_reload();
	if (alarm_state == CHUMBY_ALARM_RINGING && now - ring_started >= RING_MAX_S)
		alarm_stop();
	if (alarm_state == CHUMBY_ALARM_SNOOZED && now >= snooze_until)
		alarm_ring();

	/* Check each minute once, so a dismissed alarm doesn't ring again
	 * in the same minute. Not before the clock is set: 00:05 in 1970
	 * could match an alarm. */
	if (minute != last_minute && chumby_time_valid()) {
		last_minute = minute;
		localtime_r(&now, &tm);
		for (i = 0; i < n_alarms; i++) {
			struct chumby_alarm *a = &alarms[i];

			if (!a->enabled || a->time != tm.tm_hour * 60 + tm.tm_min ||
			    !chumby_alarm_on_day(a, &tm))
				continue;
			due = true;
			if (!a->days) {		/* "once": done */
				a->enabled = false;
				changed = true;
			}
		}
		if (changed)
			chumby_alarms_save(alarms, n_alarms);
		if (due)
			alarm_ring();
	}
	screen_update();
}

/* ------------------------------------------------------------------ */
/* Snooze button                                                      */

/* Ringing: snooze. Otherwise toggle the screen; turning it on brings up
 * the clock. No timeout either way. */
static void snooze_pressed(void)
{
	if (alarm_state == CHUMBY_ALARM_RINGING) {
		alarm_snooze();
		show_clock();
		screen_update();
		return;
	}
	wake_until = 0;
	if (screen_applied == 1) {
		screen_override = OVERRIDE_OFF;
	} else {
		screen_override = OVERRIDE_ON;
		show_clock();
	}
	screen_update();
}

static void snooze_readable(int fd, void *user)
{
	struct input_event ev;

	(void)user;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_KEY && ev.value == 1)	/* press, not repeat */
			snooze_pressed();
}

/* ------------------------------------------------------------------ */
/* Command FIFO: $run_dir/shell.cmd                                   */

static void command(const char *cmd)
{
	if (!strcmp(cmd, "snooze"))
		snooze_pressed();
	else if (!strcmp(cmd, "stop"))
		alarm_stop();
	else if (!strcmp(cmd, "ring"))
		alarm_ring();
	else if (!strcmp(cmd, "volume"))
		popup_open(POPUP_VOLUME);
	else if (!strcmp(cmd, "brightness"))
		popup_open(POPUP_BRIGHTNESS);
	else if (cmd[0])
		fprintf(stderr, "chumby-shell: unknown command '%s'\n", cmd);
}

static void cmd_readable(int fd, void *user)
{
	char buf[256], *line, *save;
	ssize_t n;

	(void)user;
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		for (line = strtok_r(buf, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save))
			command(line);
	}
}

/* Opened read-write so it never reports EOF when a writer goes away. */
static int cmd_fifo_open(void)
{
	const char *dir = chumby_run_dir();
	char path[256];

	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/shell.cmd", dir);
	if (mkfifo(path, 0622) < 0 && errno != EEXIST) {
		fprintf(stderr, "chumby-shell: cannot create %s\n", path);
		return -1;
	}
	return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

/* Reap apps we launched once they exit. */
static void reaper_cb(lv_timer_t *t)
{
	pid_t pid;

	(void)t;
	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
		int i;

		if (pid == tone_pid)
			tone_pid = -1;
		for (i = 0; i < n_apps; i++)
			if (apps[i].starting_pid == pid)
				apps[i].starting_pid = 0;	/* died starting */
	}
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
	chumby_watch_fd(find_input("snooze"), snooze_readable, NULL);
	chumby_watch_fd(find_input("mxs-lradc-ts"), touch_readable, NULL);
	chumby_watch_fd(cmd_fifo_open(), cmd_readable, NULL);
	lv_timer_ready(lv_timer_create(volume_sync_cb, 2000, NULL));
	lv_timer_create(reaper_cb, 1000, NULL);
	/* A stale state file from a previous run: we're idle. */
	alarm_publish();
	lv_timer_ready(lv_timer_create(alarm_tick_cb, 1000, NULL));

	return chumby_run();
}
