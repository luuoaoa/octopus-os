// SPDX-License-Identifier: MIT
/*
 * chumby-wm: a minimal wlroots compositor for the Chumby One's 320x240
 * touchscreen. Derived from wlroots' tinywl.
 *
 * Window policy, keyed on the xdg app_id:
 *   "chumby-panel"  the shell's tab bar: across the top, BAR_H tall,
 *                   always on top
 *   "chumby-popup@<x>"  the shell's transient controls (volume,
 *                   brightness): POPUP_W x POPUP_H, dropping down from the
 *                   bar with its right edge at x (its button's), above
 *                   apps, below the bar. A tap anywhere but on it, the
 *                   menu or the bar tells the shell ("dismiss" into
 *                   $run_dir/shell.cmd), and goes no further: popups and
 *                   the menu close, nothing under them is pressed
 *   "chumby-menu"   the shell's app menu (its Home button): a card under
 *                   the bar's left end, sized by the shell
 *   "chumby-home"   the app list, behind every app (background layer)
 *   "chumby-calibrate"  the touchscreen calibration tool: the whole
 *                   screen, above everything
 *   anything else   an app: maximized into the area under the bar, one
 *                   visible at a time (the most recently activated). The
 *                   others are frozen (SIGSTOP): nothing of theirs shows,
 *                   so they get no CPU until they're brought back
 *                   (SIGCONT), their last frame already on screen. Their
 *                   sound, if any, is another process (radio, chumby-stream).
 *                   An app with work to do out of sight opts out with
 *                   chumby_stay_awake() ($run_dir/awake/<pid>).
 *
 * Apps are exported through wlr-foreign-toplevel-management so the shell
 * can list, activate, minimize and close them. Input is touch (plus an
 * optional USB keyboard); the knob and snooze button are read by the shell
 * directly. While the shell has the backlight off (night), touches are
 * dropped so nothing gets pressed blind.
 *
 * A screen bigger than the Chumby's (a RasPad's 1280x800, say) shows the
 * 320x240 UI at the largest whole-number scale that fits, centred, pixels
 * doubled or tripled (nearest, never blurred): the output's scale, with
 * everything in a 320x240 tree in the middle of its layout. A touch is
 * mapped over the whole output, so one on the border presses nothing. A
 * screen whose own mode is huge (a 4K TV) gets one up to 1920x1200: all
 * of it is drawn by the CPU.
 *
 * A mouse (on a screen without touch, say) moves a cursor kept to the UI:
 * a translucent disc, like an iPad's, drawn at the output's scale on the
 * display's cursor plane where there is one. It shrinks while a button is
 * down, and goes after a touch or a few seconds' stillness. Apps' own
 * cursor requests are ignored.
 *
 * The screen's corners are rounded: black quarter circles over
 * everything, CORNER_R px, anti-aliased.
 *
 * Touch calibration: the matrix chumby-calibrate saves in
 * /var/lib/chumby/touch.cal, applied through libinput at start and again
 * whenever the file changes (inotify); without it, libinput's default
 * (the udev rule's LIBINPUT_CALIBRATION_MATRIX).
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <libinput.h>
#include <math.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/pixman.h>
#include "toasters.h"
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* = CHUMBY_BAR_H, CHUMBY_POPUP_W, CHUMBY_POPUP_H in chumby.h */
#define BAR_H		32
#define POPUP_W		216
#define POPUP_H		52
#define APP_PANEL	"chumby-panel"
#define APP_HOME	"chumby-home"
#define APP_POPUP	"chumby-popup"
#define APP_MENU	"chumby-menu"
#define APP_CAL		"chumby-calibrate"
#define APP_SAVER	"chumby-saver"	/* the screen saver: over everything */
#define IDLE_MS		(5 * 60 * 1000)	/* no input this long: idle */
#define CAL_DIR		"/var/lib/chumby"
#define CAL_NAME	"touch.cal"
#define CAL_FILE	CAL_DIR "/" CAL_NAME
#define MAX_TOUCH	10
#define CORNER_R	8	/* the screen's rounded corners */
#define MENU_GAP	4	/* the app menu card, from the edge and the bar */
#define UI_W		320	/* the UI's size, whatever the screen's */
#define UI_H		240
#define MODE_MAX_W	1920	/* biggest mode we'll drive (pixman, or an old Pi's GPU at */
					/* 2048 px a texture, draws it all) */
#define MODE_MAX_H	1200
#define CURSOR_R	9	/* the cursor disc's radius, in UI pixels */
#define CURSOR_MAX	64	/* cursor planes' size (vc4's): bigger is drawn in software */
#define CURSOR_IDLE_MS	5000	/* still this long: it goes */

enum view_kind { VIEW_UNKNOWN, VIEW_PANEL, VIEW_HOME, VIEW_POPUP, VIEW_MENU, VIEW_CAL,
		 VIEW_SAVER, VIEW_APP };
/* The veil (see veil_update()): off, under a popup, under the saver. */
enum veil_mode { VEIL_OFF, VEIL_POPUP, VEIL_SAVER };

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;

	/* Stacking: home < apps < veil < popup < panel < saver, all in the
	 * UI's tree (the veil goes under the saver while that's up). */
	struct wlr_scene_tree *ui_tree, *home_tree, *apps_tree, *veil_tree, *popup_tree,
		*panel_tree, *saver_tree, *cal_tree, *corner_tree;
	/* Under the shell's popup or menu, over the app (not the bar): the
	 * app blurred (on a GPU only) and dimmed. */
	bool gpu;			/* rendering on one (not pixman) */
	struct wlr_scene_buffer *veil_blur;
	struct wlr_scene_rect *veil_dim;
	struct wl_event_source *blur_timer;
	bool blur_pending;
	int veil_mode;			/* enum veil_mode */
	struct view *saver;		/* the screen saver's window, if any */
	struct toasters *toasters;	/* flying over it (toasters.c) */

	/* Nobody's touched anything for IDLE_MS: the shell is told ("idle",
	 * then "active" at the next input), and may start the saver. */
	struct wl_event_source *idle_timer;
	bool idle;
	struct wlr_scene_rect *ui_backdrop;
	bool corners;			/* made (once the output's size is known) */

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list views;		/* view.link, most recent first */

	struct wlr_foreign_toplevel_manager_v1 *ftl_manager;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_list keyboards;
	bool have_touch;
	struct wl_list touch_devs;	/* touch_dev.link */
	struct { bool active; double ox, oy; } touch[MAX_TOUCH];

	struct wl_listener new_output;
	struct wlr_output *output;	/* the LCD */
	int width, height;		/* the UI's (UI_W x UI_H on a bigger screen) */
	double layout_w, layout_h;	/* the output's, in layout (scaled) pixels */
	int scale;

	/* A mouse: its cursor. */
	struct wlr_cursor *cursor;
	struct wlr_buffer *cursor_img[2];	/* up, pressed */
	int cursor_img_scale;
	bool cursor_shown, cursor_down, cursor_swallow;
	int pointers;
	struct wl_event_source *cursor_idle;
	struct wl_listener cursor_motion, cursor_motion_abs, cursor_button, cursor_axis,
		cursor_frame;
};

struct view {
	struct wl_list link;
	struct server *server;
	enum view_kind kind;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *tree;
	struct wlr_foreign_toplevel_handle_v1 *ftl;
	bool mapped, minimized;
	bool frozen;		/* its process is stopped (an app out of sight) */
	int right;		/* a popup: its right edge (from its app_id) */

	struct wl_listener map, unmap, commit, destroy;
	struct wl_listener request_maximize, request_fullscreen, request_minimize;
	struct wl_listener set_title, set_app_id;
	struct wl_listener ftl_activate, ftl_minimize, ftl_close;
};

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers, key, destroy;
};

struct touch_dev {
	struct wl_list link;
	struct server *server;
	struct wlr_input_device *dev;
	struct wl_listener down, up, motion, frame, destroy;
};

/* ------------------------------------------------------------------ */
/* Views                                                              */

static void veil_update(struct server *s);
static void veil_app_changed(struct server *s);

static void view_configure(struct view *v)
{
	struct server *s = v->server;

	switch (v->kind) {
	case VIEW_PANEL:
		wlr_scene_node_set_position(&v->tree->node, 0, 0);
		wlr_xdg_toplevel_set_size(v->xdg, s->width, BAR_H);
		break;
	case VIEW_POPUP: {
		/* Under its button, but on screen whatever it asks. */
		int x = (v->right > 0 ? v->right : s->width) - POPUP_W;

		if (x > s->width - POPUP_W)
			x = s->width - POPUP_W;
		if (x < 0)
			x = 0;
		wlr_scene_node_set_position(&v->tree->node, x, BAR_H);
		wlr_xdg_toplevel_set_size(v->xdg, POPUP_W, POPUP_H);
		break;
	}
	case VIEW_CAL:
	case VIEW_SAVER:
		wlr_scene_node_set_position(&v->tree->node, 0, 0);
		wlr_xdg_toplevel_set_size(v->xdg, s->width, s->height);
		break;
	case VIEW_MENU:
		/* Its size is the shell's: as tall as its list of apps. A
		 * card just under the bar, clear of the screen's edge. */
		wlr_scene_node_set_position(&v->tree->node, MENU_GAP, BAR_H + MENU_GAP);
		wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
		break;
	case VIEW_HOME:
	case VIEW_APP:
		wlr_scene_node_set_position(&v->tree->node, 0, BAR_H);
		wlr_xdg_toplevel_set_size(v->xdg, s->width, s->height - BAR_H);
		wlr_xdg_toplevel_set_maximized(v->xdg, true);
		break;
	default:
		break;
	}
}

static struct view *panel_view(struct server *s)
{
	struct view *v;

	wl_list_for_each(v, &s->views, link)
		if (v->kind == VIEW_PANEL && v->mapped)
			return v;
	return NULL;
}

static struct view *top_app(struct server *s)
{
	struct view *v;

	wl_list_for_each(v, &s->views, link)
		if (v->kind == VIEW_APP && v->mapped && !v->minimized)
			return v;
	return NULL;
}

static void set_keyboard_focus(struct server *s, struct view *v)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);

	if (!v) {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
		return;
	}
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, v->xdg->base->surface,
					       kb->keycodes, kb->num_keycodes,
					       &kb->modifiers);
}

/* Stop or continue an app's process. Only apps: the shell's windows
 * (bar, menu, home) stay live. */
static void view_freeze(struct view *v, bool freeze)
{
	struct wl_client *client;
	pid_t pid;
	uid_t uid;
	gid_t gid;

	if (v->kind != VIEW_APP || v->frozen == freeze)
		return;
	client = wl_resource_get_client(v->xdg->base->resource);
	wl_client_get_credentials(client, &pid, &uid, &gid);
	if (pid <= 0 || pid == getpid())
		return;
	if (freeze) {	/* asked to keep running (chumby_stay_awake())? */
		const char *dir = getenv("CHUMBY_RUN_DIR");
		char path[256];

		snprintf(path, sizeof(path), "%s/awake/%d", dir && *dir ? dir : "/run/chumby",
			 (int)pid);
		if (access(path, F_OK) == 0)
			return;
	}
	/* Its pending events (the deactivating configure) wait in its
	 * socket; frozen views get no frame callbacks meanwhile. */
	wl_client_flush(client);
	if (kill(pid, freeze ? SIGSTOP : SIGCONT) == 0)
		v->frozen = freeze;
}

/* Make `v` the visible, active app (or show home when v is NULL). */
static void activate(struct server *s, struct view *v)
{
	struct view *o;

	if (v)
		view_freeze(v, false);
	wl_list_for_each(o, &s->views, link) {
		if (o->kind != VIEW_APP || o == v)
			continue;
		wlr_xdg_toplevel_set_activated(o->xdg, false);
		if (o->ftl)
			wlr_foreign_toplevel_handle_v1_set_activated(o->ftl, false);
		if (o->mapped && v)	/* out of sight now */
			view_freeze(o, true);
	}
	if (!v) {
		set_keyboard_focus(s, NULL);
		return;
	}

	v->minimized = false;
	wlr_scene_node_set_enabled(&v->tree->node, true);
	wlr_scene_node_raise_to_top(&v->tree->node);
	wl_list_remove(&v->link);
	wl_list_insert(&s->views, &v->link);
	wlr_xdg_toplevel_set_activated(v->xdg, true);
	if (v->ftl) {
		wlr_foreign_toplevel_handle_v1_set_minimized(v->ftl, false);
		wlr_foreign_toplevel_handle_v1_set_activated(v->ftl, true);
	}
	set_keyboard_focus(s, v);
}

static void minimize(struct view *v, bool min)
{
	struct server *s = v->server;

	if (!min) {
		activate(s, v);
		return;
	}
	v->minimized = true;
	wlr_scene_node_set_enabled(&v->tree->node, false);
	if (v->ftl) {
		wlr_foreign_toplevel_handle_v1_set_minimized(v->ftl, true);
		wlr_foreign_toplevel_handle_v1_set_activated(v->ftl, false);
	}
	activate(s, top_app(s));
}

static void ftl_activate(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, ftl_activate);

	(void)data;
	activate(v->server, v);
}

static void ftl_minimize(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, ftl_minimize);
	struct wlr_foreign_toplevel_handle_v1_minimized_event *ev = data;

	minimize(v, ev->minimized);
}

static void ftl_close(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, ftl_close);

	(void)data;
	view_freeze(v, false);		/* so it can answer */
	wlr_xdg_toplevel_send_close(v->xdg);
}

static void view_set_title(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, set_title);

	(void)data;
	if (v->ftl && v->xdg->title)
		wlr_foreign_toplevel_handle_v1_set_title(v->ftl, v->xdg->title);
}

static void view_set_app_id(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, set_app_id);

	(void)data;
	if (v->ftl && v->xdg->app_id)
		wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, v->xdg->app_id);
}

static void view_map(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, map);
	struct server *s = v->server;

	(void)data;
	v->mapped = true;
	/* The screen saver's window holds the screen; we fly the toasters
	 * over it (25 fps on a GPU, 12.5 on pixman: each frame is the
	 * ARM9's work). */
	if (v->kind == VIEW_SAVER && !s->toasters)
		s->toasters = toasters_start(s->saver_tree, wl_display_get_event_loop(s->display),
					     s->width, s->height, s->gpu ? 40 : 80, !s->gpu,
					     s->output ? s->output->render_format : 0);
	if (v->kind == VIEW_POPUP || v->kind == VIEW_MENU || v->kind == VIEW_SAVER)
		veil_update(s);
	if (v->kind != VIEW_APP)
		return;

	v->ftl = wlr_foreign_toplevel_handle_v1_create(s->ftl_manager);
	if (v->ftl) {
		if (v->xdg->title)
			wlr_foreign_toplevel_handle_v1_set_title(v->ftl, v->xdg->title);
		if (v->xdg->app_id)
			wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, v->xdg->app_id);
		wlr_foreign_toplevel_handle_v1_set_maximized(v->ftl, true);
		v->ftl_activate.notify = ftl_activate;
		wl_signal_add(&v->ftl->events.request_activate, &v->ftl_activate);
		v->ftl_minimize.notify = ftl_minimize;
		wl_signal_add(&v->ftl->events.request_minimize, &v->ftl_minimize);
		v->ftl_close.notify = ftl_close;
		wl_signal_add(&v->ftl->events.request_close, &v->ftl_close);
	}
	activate(s, v);
}

static void view_unmap(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, unmap);
	struct server *s = v->server;

	(void)data;
	v->mapped = false;
	view_freeze(v, false);
	if (v->ftl) {
		wl_list_remove(&v->ftl_activate.link);
		wl_list_remove(&v->ftl_minimize.link);
		wl_list_remove(&v->ftl_close.link);
		wlr_foreign_toplevel_handle_v1_destroy(v->ftl);
		v->ftl = NULL;
	}
	if (v->kind == VIEW_APP)
		activate(s, top_app(s));
	if (v->kind == VIEW_SAVER) {
		toasters_stop(s->toasters);
		s->toasters = NULL;
	}
	if (v->kind == VIEW_POPUP || v->kind == VIEW_MENU || v->kind == VIEW_APP ||
	    v->kind == VIEW_SAVER)
		veil_update(s);
}

static void buffer_nearest(struct wlr_scene_buffer *b, int sx, int sy, void *user)
{
	(void)sx;
	(void)sy;
	(void)user;
	wlr_scene_buffer_set_filter_mode(b, WLR_SCALE_FILTER_NEAREST);
}

static void view_commit(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, commit);
	struct server *s = v->server;
	const char *id;

	(void)data;
	/* A hidden window's commit damages nothing, so no frame would come
	 * for its callback (see frame_done_hidden). */
	if (s->output)
		wlr_output_schedule_frame(s->output);
	/* Scaled up (a bigger screen): crisp pixels, not blurred ones. */
	wlr_scene_node_for_each_buffer(&v->tree->node, buffer_nearest, NULL);
	if ((v->kind == VIEW_APP && v == top_app(s)) ||
	    (v->kind == VIEW_PANEL && s->veil_mode == VEIL_SAVER))
		veil_app_changed(s);
	if (!v->xdg->base->initial_commit)
		return;

	id = v->xdg->app_id ? v->xdg->app_id : "";
	if (!strcmp(id, APP_PANEL)) {
		v->kind = VIEW_PANEL;
		wlr_scene_node_reparent(&v->tree->node, s->panel_tree);
	} else if (!strcmp(id, APP_HOME)) {
		v->kind = VIEW_HOME;
		wlr_scene_node_reparent(&v->tree->node, s->home_tree);
	} else if (!strncmp(id, APP_POPUP, strlen(APP_POPUP))) {
		v->kind = VIEW_POPUP;
		v->right = id[strlen(APP_POPUP)] == '@' ? atoi(id + strlen(APP_POPUP) + 1) : 0;
		wlr_scene_node_reparent(&v->tree->node, s->popup_tree);
	} else if (!strcmp(id, APP_CAL)) {
		v->kind = VIEW_CAL;
		wlr_scene_node_reparent(&v->tree->node, s->cal_tree);
	} else if (!strcmp(id, APP_SAVER)) {
		v->kind = VIEW_SAVER;
		s->saver = v;
		wlr_scene_node_reparent(&v->tree->node, s->saver_tree);
	} else if (!strcmp(id, APP_MENU)) {
		v->kind = VIEW_MENU;
		wlr_scene_node_reparent(&v->tree->node, s->popup_tree);
	} else {
		v->kind = VIEW_APP;
	}
	view_configure(v);
}

/* Window geometry is policy here: ignore the client's state requests but
 * answer them with a configure, as the protocol requires. */
static void view_request_state(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, request_maximize);

	(void)data;
	if (v->xdg->base->initialized)
		wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void view_request_fullscreen(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, request_fullscreen);

	(void)data;
	if (v->xdg->base->initialized)
		wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void view_request_minimize(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, request_minimize);

	(void)data;
	if (v->kind == VIEW_APP && v->mapped)
		minimize(v, true);
}

static void view_destroy(struct wl_listener *l, void *data)
{
	struct view *v = wl_container_of(l, v, destroy);

	(void)data;
	if (v->server->saver == v)
		v->server->saver = NULL;
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_minimize.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
	wl_list_remove(&v->link);
	free(v);
}

static void new_xdg_toplevel(struct wl_listener *l, void *data)
{
	struct server *s = wl_container_of(l, s, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg = data;
	struct view *v = calloc(1, sizeof(*v));

	if (!v)
		return;
	v->server = s;
	v->xdg = xdg;
	v->tree = wlr_scene_xdg_surface_create(s->apps_tree, xdg->base);
	v->tree->node.data = v;
	xdg->base->data = v->tree;
	wl_list_insert(&s->views, &v->link);

	v->map.notify = view_map;
	wl_signal_add(&xdg->base->surface->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&xdg->base->surface->events.unmap, &v->unmap);
	v->commit.notify = view_commit;
	wl_signal_add(&xdg->base->surface->events.commit, &v->commit);
	v->destroy.notify = view_destroy;
	wl_signal_add(&xdg->events.destroy, &v->destroy);
	v->request_maximize.notify = view_request_state;
	wl_signal_add(&xdg->events.request_maximize, &v->request_maximize);
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg->events.request_fullscreen, &v->request_fullscreen);
	v->request_minimize.notify = view_request_minimize;
	wl_signal_add(&xdg->events.request_minimize, &v->request_minimize);
	v->set_title.notify = view_set_title;
	wl_signal_add(&xdg->events.set_title, &v->set_title);
	v->set_app_id.notify = view_set_app_id;
	wl_signal_add(&xdg->events.set_app_id, &v->set_app_id);
}

/* Popups (LVGL dropdowns etc.): attach to the parent's scene tree. */
struct popup {
	struct wlr_xdg_popup *xdg;
	struct wl_listener commit, destroy;
};

static void popup_commit(struct wl_listener *l, void *data)
{
	struct popup *p = wl_container_of(l, p, commit);

	(void)data;
	if (p->xdg->base->initial_commit)
		wlr_xdg_surface_schedule_configure(p->xdg->base);
}

static void popup_destroy(struct wl_listener *l, void *data)
{
	struct popup *p = wl_container_of(l, p, destroy);

	(void)data;
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void new_xdg_popup(struct wl_listener *l, void *data)
{
	struct wlr_xdg_popup *xdg = data;
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(xdg->parent);
	struct popup *p;

	(void)l;
	if (!parent)
		return;
	p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->xdg = xdg;
	xdg->base->data = wlr_scene_xdg_surface_create(parent->data, xdg->base);
	p->commit.notify = popup_commit;
	wl_signal_add(&xdg->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&xdg->events.destroy, &p->destroy);
}

/* ------------------------------------------------------------------ */
/* Input                                                              */

static struct wlr_surface *surface_at(struct server *s, double lx, double ly,
				      double *sx, double *sy)
{
	struct wlr_scene_node *node =
		wlr_scene_node_at(&s->scene->tree.node, lx, ly, sx, sy);
	struct wlr_scene_surface *ss;

	if (!node || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	return ss ? ss->surface : NULL;
}

/* The shell powers the backlight down at night (bl_power != 0). */
#define BL_POWER "/sys/class/backlight/backlight/bl_power"

static bool screen_dark(void)
{
	int fd = open(BL_POWER, O_RDONLY | O_CLOEXEC);
	char c = '0';

	if (fd < 0)
		return false;
	if (read(fd, &c, 1) != 1)
		c = '0';
	close(fd);
	return c != '0';
}

/* The shell's popup or menu is up: a tap outside it and the bar closes
 * them (and presses nothing else). */
static bool shell_popup_up(struct server *s)
{
	struct view *v;

	wl_list_for_each(v, &s->views, link)
		if (v->mapped && (v->kind == VIEW_POPUP || v->kind == VIEW_MENU))
			return true;
	return false;
}

static bool in_tree(struct wlr_scene_node *node, struct wlr_scene_tree *tree)
{
	for (; node; node = node->parent ? &node->parent->node : NULL)
		if (node == &tree->node)
			return true;
	return false;
}

/* A line to the shell's command FIFO ("dismiss", "idle", "active"). */
static void shell_cmd(const char *line)
{
	const char *dir = getenv("CHUMBY_RUN_DIR");
	char path[256], buf[64];
	int fd, n;

	snprintf(path, sizeof(path), "%s/shell.cmd", dir && *dir ? dir : "/run/chumby");
	fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	n = snprintf(buf, sizeof(buf), "%s\n", line);
	if (write(fd, buf, n) < 0)
		wlr_log(WLR_ERROR, "%s: %s", path, strerror(errno));
	close(fd);
}

static void shell_dismiss(void)
{
	shell_cmd("dismiss");
}

/* Someone's there: the idle clock starts over (and the shell hears they're
 * back, if it was told they'd gone: it closes the saver). */
static void input_activity(struct server *s)
{
	if (s->idle) {
		s->idle = false;
		shell_cmd("active");
	}
	wl_event_source_timer_update(s->idle_timer, IDLE_MS);
}

static int idle_timer_cb(void *data)
{
	struct server *s = data;

	s->idle = true;
	shell_cmd("idle");
	return 0;
}

static struct wlr_buffer *disc_create(int side, double r, double a);

static void cursor_hide(struct server *s)
{
	if (!s->cursor || !s->cursor_shown)
		return;
	wlr_cursor_unset_image(s->cursor);
	s->cursor_shown = false;
}

/* Its half-side, in UI pixels: the disc and a pixel round it, the whole
 * fitting the cursor plane (64 device pixels) - at 4x it's a bit smaller. */
static int cursor_half(int sc)
{
	int h = CURSOR_R + 1;

	return h * 2 * sc > CURSOR_MAX ? CURSOR_MAX / (2 * sc) : h;
}

static void cursor_images(struct server *s)
{
	int sc = s->scale > 0 ? s->scale : 1, h = cursor_half(sc), i;

	if (s->cursor_img_scale == sc && s->cursor_img[0])
		return;
	for (i = 0; i < 2; i++)
		if (s->cursor_img[i])
			wlr_buffer_drop(s->cursor_img[i]);
	s->cursor_img[0] = disc_create(2 * h * sc, (h - 1) * sc, 0.38);
	s->cursor_img[1] = disc_create(2 * h * sc, (h - 1) * sc * 0.75, 0.55);
	s->cursor_img_scale = sc;
}

/* Shown (as it is now: up or pressed); gone again after a while still. */
static void cursor_show(struct server *s)
{
	int sc;

	if (!s->cursor)
		return;
	cursor_images(s);
	sc = s->cursor_img_scale;
	/* The hotspot (its centre) in logical pixels, the buffer at our scale. */
	if (s->cursor_img[s->cursor_down])
		wlr_cursor_set_buffer(s->cursor, s->cursor_img[s->cursor_down],
				      cursor_half(sc), cursor_half(sc), sc);
	s->cursor_shown = true;
	wl_event_source_timer_update(s->cursor_idle, CURSOR_IDLE_MS);
}

static int cursor_idle_cb(void *data)
{
	struct server *s = data;

	if (!s->cursor_down)
		cursor_hide(s);
	return 0;
}

/* The pointer's surface (if any) gets it, where it is. */
static void cursor_moved(struct server *s, uint32_t time)
{
	double sx, sy;
	struct wlr_surface *surface = surface_at(s, s->cursor->x, s->cursor->y, &sx, &sy);

	cursor_show(s);
	if (!surface) {
		wlr_seat_pointer_clear_focus(s->seat);
		return;
	}
	wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
}

static void cursor_motion(struct wl_listener *l, void *data)
{
	struct server *s = wl_container_of(l, s, cursor_motion);
	struct wlr_pointer_motion_event *ev = data;

	input_activity(s);
	wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	cursor_moved(s, ev->time_msec);
}

static void cursor_motion_abs(struct wl_listener *l, void *data)
{
	struct server *s = wl_container_of(l, s, cursor_motion_abs);
	struct wlr_pointer_motion_absolute_event *ev = data;

	input_activity(s);
	wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
	cursor_moved(s, ev->time_msec);
}

static bool shell_popup_up(struct server *s);
static bool in_tree(struct wlr_scene_node *node, struct wlr_scene_tree *tree);
static void shell_dismiss(void);

static void cursor_button(struct wl_listener *l, void *data)
{
	struct server *s = wl_container_of(l, s, cursor_button);
	struct wlr_pointer_button_event *ev = data;
	bool down = ev->state == WL_POINTER_BUTTON_STATE_PRESSED;

	input_activity(s);
	if (screen_dark())
		return;		/* as for touches: nothing pressed blind */
	s->cursor_down = down;
	cursor_show(s);
	/* Like a tap: outside the shell's popup or menu (and the bar), a
	 * click closes them and goes no further - its release neither. */
	if (down && shell_popup_up(s)) {
		struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node,
								s->cursor->x, s->cursor->y,
								NULL, NULL);

		if (!in_tree(node, s->popup_tree) && !in_tree(node, s->panel_tree) &&
		    !in_tree(node, s->cal_tree)) {
			shell_dismiss();
			s->cursor_swallow = true;
			return;
		}
	}
	if (!down && s->cursor_swallow) {
		s->cursor_swallow = false;
		return;
	}
	wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
}

static void cursor_axis(struct wl_listener *l, void *data)
{
	struct server *s = wl_container_of(l, s, cursor_axis);
	struct wlr_pointer_axis_event *ev = data;

	input_activity(s);
	cursor_show(s);
	wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation, ev->delta,
				     ev->delta_discrete, ev->source, ev->relative_direction);
}

static void cursor_frame(struct wl_listener *l, void *data)
{
	struct server *s = wl_container_of(l, s, cursor_frame);

	(void)data;
	wlr_seat_pointer_notify_frame(s->seat);
}

/* Kept to the UI (not the black border), and to the middle at first. */
static void cursor_fit(struct server *s)
{
	struct wlr_box box;

	if (!s->cursor)
		return;
	box.x = s->ui_tree->node.x;
	box.y = s->ui_tree->node.y;
	box.width = s->width;
	box.height = s->height;
	wlr_cursor_map_to_region(s->cursor, &box);
	wlr_cursor_warp(s->cursor, NULL, box.x + box.width / 2.0, box.y + box.height / 2.0);
	s->cursor_img_scale = 0;	/* redrawn for the new scale */
	if (s->cursor_shown)
		cursor_show(s);
}

static void touch_down(struct wl_listener *l, void *data)
{
	struct touch_dev *t = wl_container_of(l, t, down);
	struct server *s = t->server;
	struct wlr_touch_down_event *ev = data;
	double lx = ev->x * s->layout_w, ly = ev->y * s->layout_h, sx, sy;
	struct wlr_surface *surface = surface_at(s, lx, ly, &sx, &sy);

	input_activity(s);
	/* Not marked active: its motion and up are dropped too. */
	if (ev->touch_id < 0 || ev->touch_id >= MAX_TOUCH || screen_dark())
		return;
	cursor_hide(s);		/* a finger now, not the mouse */
	if (shell_popup_up(s)) {
		struct wlr_scene_node *node =
			wlr_scene_node_at(&s->scene->tree.node, lx, ly, NULL, NULL);

		if (!in_tree(node, s->popup_tree) && !in_tree(node, s->panel_tree) &&
		    !in_tree(node, s->cal_tree)) {
			shell_dismiss();
			return;
		}
	}
	if (!surface)
		return;
	/* Remember the surface origin so motion stays relative to it. */
	s->touch[ev->touch_id].active = true;
	s->touch[ev->touch_id].ox = lx - sx;
	s->touch[ev->touch_id].oy = ly - sy;
	wlr_seat_touch_notify_down(s->seat, surface, ev->time_msec,
				   ev->touch_id, sx, sy);
}

static void touch_motion(struct wl_listener *l, void *data)
{
	struct touch_dev *t = wl_container_of(l, t, motion);
	struct server *s = t->server;
	struct wlr_touch_motion_event *ev = data;

	if (ev->touch_id < 0 || ev->touch_id >= MAX_TOUCH ||
	    !s->touch[ev->touch_id].active)
		return;
	wlr_seat_touch_notify_motion(s->seat, ev->time_msec, ev->touch_id,
				     ev->x * s->layout_w - s->touch[ev->touch_id].ox,
				     ev->y * s->layout_h - s->touch[ev->touch_id].oy);
}

static void touch_up(struct wl_listener *l, void *data)
{
	struct touch_dev *t = wl_container_of(l, t, up);
	struct server *s = t->server;
	struct wlr_touch_up_event *ev = data;

	if (ev->touch_id < 0 || ev->touch_id >= MAX_TOUCH ||
	    !s->touch[ev->touch_id].active)
		return;
	s->touch[ev->touch_id].active = false;
	wlr_seat_touch_notify_up(s->seat, ev->time_msec, ev->touch_id);
}

static void touch_frame(struct wl_listener *l, void *data)
{
	struct touch_dev *t = wl_container_of(l, t, frame);

	(void)data;
	wlr_seat_touch_notify_frame(t->server->seat);
}

static void touch_destroy(struct wl_listener *l, void *data)
{
	struct touch_dev *t = wl_container_of(l, t, destroy);

	(void)data;
	wl_list_remove(&t->down.link);
	wl_list_remove(&t->up.link);
	wl_list_remove(&t->motion.link);
	wl_list_remove(&t->frame.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

/* The saved calibration, else libinput's default (from udev). */
static void cal_apply(struct wlr_input_device *dev)
{
	struct libinput_device *ld;
	float m[6];
	FILE *f;

	if (!wlr_input_device_is_libinput(dev))
		return;
	ld = wlr_libinput_get_device_handle(dev);
	if (!ld || !libinput_device_config_calibration_has_matrix(ld))
		return;
	f = fopen(CAL_FILE, "r");
	if (!f || fscanf(f, "%f %f %f %f %f %f", &m[0], &m[1], &m[2],
			 &m[3], &m[4], &m[5]) != 6)
		libinput_device_config_calibration_get_default_matrix(ld, m);
	if (f)
		fclose(f);
	if (libinput_device_config_calibration_set_matrix(ld, m) == LIBINPUT_CONFIG_STATUS_SUCCESS)
		wlr_log(WLR_INFO, "touch calibration %.4f %.4f %.4f %.4f %.4f %.4f",
			m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int cal_changed(int fd, uint32_t mask, void *data)
{
	struct server *s = data;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *ev;
	struct touch_dev *t;
	bool ours = false;
	ssize_t n;
	char *p;

	(void)mask;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		for (p = buf; p < buf + n; p += sizeof(*ev) + ev->len) {
			ev = (const struct inotify_event *)p;
			if (ev->len && !strcmp(ev->name, CAL_NAME))
				ours = true;
		}
	if (ours)
		wl_list_for_each(t, &s->touch_devs, link)
			cal_apply(t->dev);
	return 0;
}

static void new_touch(struct server *s, struct wlr_input_device *dev)
{
	struct wlr_touch *touch = wlr_touch_from_input_device(dev);
	struct touch_dev *t = calloc(1, sizeof(*t));

	if (!t)
		return;
	t->server = s;
	t->dev = dev;
	wl_list_insert(&s->touch_devs, &t->link);
	cal_apply(dev);
	t->down.notify = touch_down;
	wl_signal_add(&touch->events.down, &t->down);
	t->up.notify = touch_up;
	wl_signal_add(&touch->events.up, &t->up);
	t->motion.notify = touch_motion;
	wl_signal_add(&touch->events.motion, &t->motion);
	t->frame.notify = touch_frame;
	wl_signal_add(&touch->events.frame, &t->frame);
	t->destroy.notify = touch_destroy;
	wl_signal_add(&dev->events.destroy, &t->destroy);
	s->have_touch = true;
}

static void kb_modifiers(struct wl_listener *l, void *data)
{
	struct keyboard *k = wl_container_of(l, k, modifiers);

	(void)data;
	wlr_seat_set_keyboard(k->server->seat, k->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(k->server->seat,
					   &k->wlr_keyboard->modifiers);
}

static void kb_key(struct wl_listener *l, void *data)
{
	struct keyboard *k = wl_container_of(l, k, key);
	struct wlr_keyboard_key_event *ev = data;

	input_activity(k->server);
	wlr_seat_set_keyboard(k->server->seat, k->wlr_keyboard);
	wlr_seat_keyboard_notify_key(k->server->seat, ev->time_msec,
				     ev->keycode, ev->state);
}

static void kb_destroy(struct wl_listener *l, void *data)
{
	struct keyboard *k = wl_container_of(l, k, destroy);

	(void)data;
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	free(k);
}

static void new_keyboard(struct server *s, struct wlr_input_device *dev)
{
	struct wlr_keyboard *wkb = wlr_keyboard_from_input_device(dev);
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = ctx ?
		xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
	struct keyboard *k;

	/* The snooze button (gpio-keys "snooze") belongs to the shell. */
	if (dev->name && !strcmp(dev->name, "snooze")) {
		if (keymap)
			xkb_keymap_unref(keymap);
		if (ctx)
			xkb_context_unref(ctx);
		return;
	}
	/* No xkeyboard-config data on the image: no keyboards. */
	if (!keymap) {
		wlr_log(WLR_INFO, "no XKB keymap available, ignoring keyboard %s",
			dev->name);
		if (ctx)
			xkb_context_unref(ctx);
		return;
	}
	k = calloc(1, sizeof(*k));
	if (!k)
		goto out;
	k->server = s;
	k->wlr_keyboard = wkb;
	wlr_keyboard_set_keymap(wkb, keymap);
	wlr_keyboard_set_repeat_info(wkb, 25, 600);
	k->modifiers.notify = kb_modifiers;
	wl_signal_add(&wkb->events.modifiers, &k->modifiers);
	k->key.notify = kb_key;
	wl_signal_add(&wkb->events.key, &k->key);
	k->destroy.notify = kb_destroy;
	wl_signal_add(&dev->events.destroy, &k->destroy);
	wlr_seat_set_keyboard(s->seat, wkb);
	wl_list_insert(&s->keyboards, &k->link);
out:
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
}

static void new_input(struct wl_listener *l, void *data)
{
	struct server *s = wl_container_of(l, s, new_input);
	struct wlr_input_device *dev = data;
	uint32_t caps = 0;

	switch (dev->type) {
	case WLR_INPUT_DEVICE_TOUCH:
		new_touch(s, dev);
		break;
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(s, dev);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(s->cursor, dev);
		s->pointers++;
		break;
	default:
		break;
	}
	if (s->have_touch)
		caps |= WL_SEAT_CAPABILITY_TOUCH;
	if (s->pointers)
		caps |= WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}

/* ------------------------------------------------------------------ */
/* Output                                                             */

struct output {
	struct server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame, request_state, destroy;
};

static void frame_done_iter(struct wlr_surface *surface, int sx, int sy, void *data)
{
	(void)sx; (void)sy;
	wlr_surface_send_frame_done(surface, data);
}

/*
 * The scene sends frame callbacks only to surfaces it shows. LVGL's
 * Wayland backend blocks after each flush until its frame callback comes
 * (dispatching events, but running no timers or input), so a window
 * drawing while hidden (the shell's home under an app, an app behind
 * another) would hang its whole client: the shell's dock went dead. Pace
 * hidden windows by the output too. (Callbacks already sent are gone,
 * so visible surfaces get no second one.)
 */
static void frame_done_hidden(struct server *s, const struct timespec *now)
{
	struct view *v;

	wl_list_for_each(v, &s->views, link)
		if (v->mapped && !v->frozen)
			wlr_xdg_surface_for_each_surface(v->xdg->base, frame_done_iter,
							 (void *)now);
}

static void output_frame(struct wl_listener *l, void *data)
{
	struct output *o = wl_container_of(l, o, frame);
	struct wlr_scene_output *so =
		wlr_scene_get_scene_output(o->server->scene, o->wlr_output);
	struct timespec now;

	static int debug = -1;
	static long frames, us;
	static struct timespec since;
	struct timespec t0;

	(void)data;
	if (debug < 0)
		debug = getenv("CHUMBY_WM_DEBUG") != NULL;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	wlr_scene_output_commit(so, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
	/* CHUMBY_WM_DEBUG=1: frames a second and the time they took. */
	if (debug) {
		frames++;
		us += (now.tv_sec - t0.tv_sec) * 1000000 + (now.tv_nsec - t0.tv_nsec) / 1000;
		if (now.tv_sec - since.tv_sec >= 1) {
			wlr_log(WLR_INFO, "frames: %ld/s, %ld us", frames, us);
			frames = us = 0;
			since = now;
		}
	}
	wlr_scene_output_send_frame_done(so, &now);
	frame_done_hidden(o->server, &now);
}

static void output_request_state(struct wl_listener *l, void *data)
{
	struct output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;

	wlr_output_commit_state(o->wlr_output, ev->state);
}

static void output_destroy(struct wl_listener *l, void *data)
{
	struct output *o = wl_container_of(l, o, destroy);

	(void)data;
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	if (o->server->output == o->wlr_output)
		o->server->output = NULL;
	free(o);
}

/* A read-only pixel buffer: the top-left corner's mask, black outside a
 * quarter circle (premultiplied ARGB, so just alpha). */
/* A buffer of premultiplied pixels: the cursor's discs (ARGB), the
 * veil's blurred app (as the GPU reads it back). */
struct pix_buf {
	struct wlr_buffer base;
	int w, h;
	uint32_t format;
	uint32_t px[];
};

static void pix_destroy(struct wlr_buffer *b)
{
	free(wl_container_of(b, (struct pix_buf *)NULL, base));
}

static bool pix_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format,
		      size_t *stride)
{
	struct pix_buf *pb = wl_container_of(b, pb, base);

	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = pb->px;
	*format = pb->format;
	*stride = pb->w * 4;
	return true;
}

static void pix_end(struct wlr_buffer *b)
{
	(void)b;
}

static const struct wlr_buffer_impl pix_impl = {
	.destroy = pix_destroy,
	.begin_data_ptr_access = pix_begin,
	.end_data_ptr_access = pix_end,
};

/* A soft disc of radius r (device pixels), centred in a side x side
 * square: a dark grey fill, alpha a, with a thin light rim so it shows
 * on dark screens too. */
static struct wlr_buffer *disc_create(int side, double r, double a)
{
	struct pix_buf *pb = calloc(1, sizeof(*pb) + sizeof(uint32_t) * side * side);
	double c = side / 2.0, rim = r > 6 ? r / 9 : 1;
	int x, y;

	if (!pb)
		return NULL;
	pb->w = pb->h = side;
	pb->format = DRM_FORMAT_ARGB8888;
	wlr_buffer_init(&pb->base, &pix_impl, side, side);
	for (y = 0; y < side; y++)
		for (x = 0; x < side; x++) {
			double d = hypot(x + 0.5 - c, y + 0.5 - c);
			double cover = r + 0.5 - d;		/* antialiased edge */
			double inner = (r - rim) + 0.5 - d;
			double al, grey;

			cover = cover < 0 ? 0 : cover > 1 ? 1 : cover;
			inner = inner < 0 ? 0 : inner > 1 ? 1 : inner;
			/* rim: light, a bit more opaque; inside: dark grey */
			al = cover * (inner * a + (1 - inner) * (a + 0.25));
			grey = inner * 0.22 + (1 - inner) * 0.92;
			pb->px[y * side + x] = (uint32_t)(al * 255 + 0.5) << 24 |
					       (uint32_t)(grey * al * 255 + 0.5) * 0x010101u;
		}
	return &pb->base;
}

/* ------------------------------------------------------------------ */
/* The veil: under the shell's popups (over the app), or under the    */
/* screen saver (over everything)                                     */

#define BLUR_R		4	/* box radius, UI pixels; three passes ~ Gaussian */
#define BLUR_PASSES	3
#define BLUR_EVERY_MS	250	/* what's under it changing: blurred again, at most (~10 ms each on a Pi 4) */
#define DIM_GPU		0.22f	/* black over the blur */
#define DIM_SOFT	0.30f	/* black over the app, no blur (pixman) */
#define DIM_SAVER_GPU	0.45f	/* under the screen saver: the toasters stand out */
#define DIM_SAVER_SOFT	0.60f

/* One box-blur pass along a line of n pixels, `step` apart, all four
 * channels: a running sum over 2r+1, the edges repeated. */
static void blur_line(uint32_t *p, int n, int step, int r, uint32_t *tmp)
{
	uint32_t sum[4] = { 0 };
	int i, c, k = 2 * r + 1;

	for (i = 0; i < n; i++)
		tmp[i] = p[i * step];
	for (i = -r; i <= r; i++) {
		uint32_t v = tmp[i < 0 ? 0 : i >= n ? n - 1 : i];

		for (c = 0; c < 4; c++)
			sum[c] += v >> (c * 8) & 0xff;
	}
	for (i = 0; i < n; i++) {
		uint32_t out = 0, in = tmp[i + r + 1 < n ? i + r + 1 : n - 1],
			 gone = tmp[i - r > 0 ? i - r : 0];

		for (c = 0; c < 4; c++) {
			out |= (sum[c] / k) << (c * 8);
			sum[c] += (in >> (c * 8) & 0xff) - (gone >> (c * 8) & 0xff);
		}
		p[i * step] = out;
	}
}

/* A window's picture into pb at row y (read back from its texture: a
 * GPU's, so it's cheap at our size), in pb's format. */
static void veil_read(struct pix_buf *pb, struct view *v, int y)
{
	struct wlr_texture *tex = v && v->mapped ?
		wlr_surface_get_texture(v->xdg->base->surface) : NULL;
	int w, h;

	if (!tex || y >= pb->h)
		return;
	w = tex->width < (uint32_t)pb->w ? (int)tex->width : pb->w;
	h = tex->height < (uint32_t)(pb->h - y) ? (int)tex->height : pb->h - y;
	wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
		.data = pb->px, .format = pb->format, .stride = pb->w * 4,
		.dst_x = 0, .dst_y = y, .src_box = { 0, 0, w, h } });
}

static struct view *top_app(struct server *s);
static struct view *panel_view(struct server *s);

/* What's under the veil as it is now, blurred, as the veil's buffer:
 * the app (popups), or the bar and the app (the saver). */
static void veil_blur_now(struct server *s)
{
	struct view *app = top_app(s);
	struct wlr_texture *tex = app ? wlr_surface_get_texture(app->xdg->base->surface) : NULL;
	bool saver = s->veil_mode == VEIL_SAVER;
	int w = s->width, h = saver ? s->height : s->height - BAR_H, x, y, pass;
	struct pix_buf *pb;
	uint32_t *tmp;

	s->blur_pending = false;
	pb = calloc(1, sizeof(*pb) + sizeof(uint32_t) * w * h);
	tmp = malloc(sizeof(uint32_t) * (w > h ? w : h));
	if (!pb || !tmp)
		goto fail;
	pb->w = w;
	pb->h = h;
	pb->format = tex ? wlr_texture_preferred_read_format(tex) : DRM_FORMAT_XRGB8888;
	if (pb->format != DRM_FORMAT_XRGB8888 && pb->format != DRM_FORMAT_ARGB8888 &&
	    pb->format != DRM_FORMAT_XBGR8888 && pb->format != DRM_FORMAT_ABGR8888)
		pb->format = DRM_FORMAT_ARGB8888;
	if (saver)
		veil_read(pb, panel_view(s), 0);
	veil_read(pb, app, saver ? BAR_H : 0);
	for (pass = 0; pass < BLUR_PASSES; pass++) {
		for (y = 0; y < h; y++)
			blur_line(pb->px + y * w, w, 1, BLUR_R, tmp);
		for (x = 0; x < w; x++)
			blur_line(pb->px + x, h, w, BLUR_R, tmp);
	}
	for (x = 0; x < w * h; x++)	/* opaque (X formats: undefined) */
		pb->px[x] |= 0xff000000u;
	free(tmp);
	wlr_buffer_init(&pb->base, &pix_impl, w, h);
	wlr_scene_buffer_set_buffer(s->veil_blur, &pb->base);
	wlr_buffer_drop(&pb->base);
	/* Blurred already: smooth when scaled up, not blocky. */
	wlr_scene_buffer_set_filter_mode(s->veil_blur, WLR_SCALE_FILTER_BILINEAR);
	wlr_scene_buffer_set_dest_size(s->veil_blur, w, h);
	return;
fail:
	free(tmp);
	free(pb);
}

static bool shell_popup_up(struct server *s);

static int blur_timer_cb(void *data)
{
	struct server *s = data;

	if (s->blur_pending && s->veil_mode != VEIL_OFF)
		veil_blur_now(s);
	s->blur_pending = false;
	return 0;
}

/* Under the saver while it's up (over the bar too); else under the
 * shell's popup or menu, over the app in front; else gone. */
static void veil_update(struct server *s)
{
	struct view *app = top_app(s);
	enum veil_mode mode = s->saver && s->saver->mapped ? VEIL_SAVER :
			      app && shell_popup_up(s) ? VEIL_POPUP : VEIL_OFF;
	bool changed = mode != s->veil_mode;
	bool gpu = s->gpu;

	/* The app under the saver is out of sight: stopped like one behind
	 * another (its redraws, and the frames they'd cost us, with it);
	 * going again when the saver goes. (Its sound plays on: that's
	 * chumby-stream's, the radio's, not the app's.) */
	if (app && changed && (mode == VEIL_SAVER || s->veil_mode == VEIL_SAVER))
		view_freeze(app, mode == VEIL_SAVER);
	s->veil_mode = mode;
	wlr_scene_node_set_enabled(&s->veil_tree->node, mode != VEIL_OFF);
	if (mode == VEIL_OFF) {
		if (s->veil_blur)
			wlr_scene_buffer_set_buffer(s->veil_blur, NULL);	/* its memory */
		return;
	}
	if (mode == VEIL_SAVER) {
		wlr_scene_node_place_below(&s->veil_tree->node, &s->saver_tree->node);
		wlr_scene_node_set_position(&s->veil_tree->node, 0, 0);
		wlr_scene_rect_set_size(s->veil_dim, s->width, s->height);
		wlr_scene_rect_set_color(s->veil_dim, (float[4]){ 0, 0, 0,
					 gpu ? DIM_SAVER_GPU : DIM_SAVER_SOFT });
	} else {
		wlr_scene_node_place_above(&s->veil_tree->node, &s->apps_tree->node);
		wlr_scene_node_set_position(&s->veil_tree->node, 0, BAR_H);
		wlr_scene_rect_set_size(s->veil_dim, s->width, s->height - BAR_H);
		wlr_scene_rect_set_color(s->veil_dim, (float[4]){ 0, 0, 0,
					 gpu ? DIM_GPU : DIM_SOFT });
	}
	if (gpu && changed)
		veil_blur_now(s);
}

/* What's under the veil drew: blur it again, soon (not at its every
 * frame). */
static void veil_app_changed(struct server *s)
{
	if (!s->gpu || s->veil_mode == VEIL_OFF || s->blur_pending)
		return;
	s->blur_pending = true;
	wl_event_source_timer_update(s->blur_timer, BLUR_EVERY_MS);
}

struct corner_buf {
	struct wlr_buffer base;
	uint32_t px[CORNER_R * CORNER_R];
};

static void corner_destroy(struct wlr_buffer *b)
{
	free(wl_container_of(b, (struct corner_buf *)NULL, base));
}

static bool corner_begin(struct wlr_buffer *b, uint32_t flags, void **data,
			 uint32_t *format, size_t *stride)
{
	struct corner_buf *c = wl_container_of(b, c, base);

	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = c->px;
	*format = DRM_FORMAT_ARGB8888;
	*stride = CORNER_R * 4;
	return true;
}

static void corner_end(struct wlr_buffer *b)
{
	(void)b;
}

static const struct wlr_buffer_impl corner_impl = {
	.destroy = corner_destroy,
	.begin_data_ptr_access = corner_begin,
	.end_data_ptr_access = corner_end,
};

/*
 * Anything bound over the read-only image (/usr, /etc, /bin, /sbin, /lib:
 * a run overlay, bin/chumby-try's builds) and the screen gets a magenta
 * frame over everything: you're not running the image, and an overlay left
 * behind after the experiments shows. The files go to the journal.
 */
#define OVERLAY_FRAME	2	/* UI pixels */

static bool image_overlaid(void)
{
	static const char *const image[] = { "/usr/", "/etc/", "/bin/", "/sbin/", "/lib/" };
	FILE *f = fopen("/proc/self/mountinfo", "r");
	char line[1024], from[256], where[256];
	bool any = false;
	size_t i;

	if (!f)
		return false;
	/* A bind from the SD card (its /data: where overlays keep their
	 * files), not systemd's own (/etc/machine-id, from a tmpfs). */
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%*s %*s %*s %255s %255s", from, where) == 2 &&
		    !strncmp(from, "/data/", 6))
			for (i = 0; i < sizeof(image) / sizeof(image[0]); i++)
				if (!strncmp(where, image[i], strlen(image[i]))) {
					wlr_log(WLR_INFO, "run overlay: %s (from the card's %s)", where, from);
					any = true;
				}
	fclose(f);
	return any;
}

static void overlay_mark_create(struct server *s)
{
	static const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
	int w = s->width, h = s->height, t = OVERLAY_FRAME;
	struct wlr_scene_rect *r;

	if (!image_overlaid())
		return;
	r = wlr_scene_rect_create(s->corner_tree, w, t, magenta);
	wlr_scene_node_set_position(&r->node, 0, 0);
	r = wlr_scene_rect_create(s->corner_tree, w, t, magenta);
	wlr_scene_node_set_position(&r->node, 0, h - t);
	r = wlr_scene_rect_create(s->corner_tree, t, h - 2 * t, magenta);
	wlr_scene_node_set_position(&r->node, 0, t);
	r = wlr_scene_rect_create(s->corner_tree, t, h - 2 * t, magenta);
	wlr_scene_node_set_position(&r->node, w - t, t);
}

/* Four of them over everything, the one mask mirrored into each corner. */
static void corners_create(struct server *s)
{
	static const struct {
		enum wl_output_transform t;
		int right, bottom;
	} where[4] = {
		{ WL_OUTPUT_TRANSFORM_NORMAL, 0, 0 },
		{ WL_OUTPUT_TRANSFORM_FLIPPED, 1, 0 },
		{ WL_OUTPUT_TRANSFORM_FLIPPED_180, 0, 1 },
		{ WL_OUTPUT_TRANSFORM_180, 1, 1 },
	};
	struct corner_buf *c;
	int x, y, i;

	if (s->corners || !s->width)
		return;
	c = calloc(1, sizeof(*c));
	if (!c)
		return;
	wlr_buffer_init(&c->base, &corner_impl, CORNER_R, CORNER_R);
	for (y = 0; y < CORNER_R; y++)
		for (x = 0; x < CORNER_R; x++) {
			double d = hypot(CORNER_R - (x + 0.5), CORNER_R - (y + 0.5));
			double a = d - CORNER_R + 0.5;	/* coverage outside the arc */

			a = a < 0 ? 0 : a > 1 ? 1 : a;
			c->px[y * CORNER_R + x] = (uint32_t)(a * 255 + 0.5) << 24;
		}
	for (i = 0; i < 4; i++) {
		struct wlr_scene_buffer *b = wlr_scene_buffer_create(s->corner_tree, &c->base);

		if (!b)
			continue;
		wlr_scene_buffer_set_transform(b, where[i].t);
		wlr_scene_buffer_set_filter_mode(b, WLR_SCALE_FILTER_NEAREST);
		wlr_scene_node_set_position(&b->node, where[i].right ? s->width - CORNER_R : 0,
					    where[i].bottom ? s->height - CORNER_R : 0);
	}
	wlr_buffer_drop(&c->base);	/* the scene buffers hold it now */
	s->corners = true;
	overlay_mark_create(s);		/* (once, as the corners) */
}

/* The screen's own mode, unless it's bigger than we'd want to draw every
 * pixel of in software (a 4K TV): then its biggest one that isn't, at the
 * refresh nearest 60 Hz. */
static struct wlr_output_mode *pick_mode(struct wlr_output *wo)
{
	struct wlr_output_mode *pref = wlr_output_preferred_mode(wo), *m, *best = NULL;

	if (!pref || (pref->width <= MODE_MAX_W && pref->height <= MODE_MAX_H))
		return pref;
	wl_list_for_each(m, &wo->modes, link) {
		long area = (long)m->width * m->height, best_area;

		if (m->width > MODE_MAX_W || m->height > MODE_MAX_H)
			continue;
		best_area = best ? (long)best->width * best->height : -1;
		if (area > best_area || (area == best_area &&
		    labs(m->refresh - 60000) < labs(best->refresh - 60000)))
			best = m;
	}
	return best ? best : pref;
}

static void new_output(struct wl_listener *l, void *data)
{
	struct server *s = wl_container_of(l, s, new_output);
	struct wlr_output *wo = data;
	struct wlr_output_state state;
	struct wlr_output_mode *mode;
	struct wlr_output_layout_output *lo;
	struct wlr_scene_output *so;
	struct output *o;
	struct view *v;
	int pw, ph, scale;

	wlr_output_init_render(wo, s->allocator, s->renderer);
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	mode = pick_mode(wo);
	if (mode) {
		pw = mode->width;
		ph = mode->height;
		wlr_output_state_set_mode(&state, mode);
	} else {	/* modeless (headless, for testing): the LCD's size, or
			 * CHUMBY_WM_SIZE=<w>x<h> to try a bigger screen */
		const char *size = getenv("CHUMBY_WM_SIZE");

		if (!size || sscanf(size, "%dx%d", &pw, &ph) != 2 || pw <= 0 || ph <= 0) {
			pw = UI_W;
			ph = UI_H;
		}
		wlr_output_state_set_custom_mode(&state, pw, ph, 0);
	}
	/* The UI at the biggest whole scale that fits. */
	scale = pw / UI_W < ph / UI_H ? pw / UI_W : ph / UI_H;
	if (scale < 1)
		scale = 1;
	wlr_output_state_set_scale(&state, scale);
	/* Without a GPU, a 16-bit screen, if it takes one: the apps draw
	 * 16-bit, so showing them is a plain copy, and every copy (pixman's,
	 * the LCD controller's scanning out) moves half the bytes - which,
	 * through the ARM9's slow, uncached memory, is most of the cost. */
	if (!s->gpu && !getenv("CHUMBY_WM_32BIT")) {
		wlr_output_state_set_render_format(&state, DRM_FORMAT_RGB565);
		if (!wlr_output_test_state(wo, &state))
			wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB8888);
	}
	wlr_output_commit_state(wo, &state);
	wlr_output_state_finish(&state);

	s->layout_w = (double)wo->width / scale;
	s->layout_h = (double)wo->height / scale;
	if (scale > 1 || wo->width > UI_W || wo->height > UI_H) {
		s->width = UI_W;
		s->height = UI_H;
	} else {	/* the Chumby's own LCD (or something smaller) */
		s->width = wo->width;
		s->height = wo->height;
	}
	/* Centred, black around it. */
	wlr_scene_node_set_position(&s->ui_tree->node, (int)((s->layout_w - s->width) / 2),
				    (int)((s->layout_h - s->height) / 2));
	wlr_scene_rect_set_size(s->ui_backdrop, s->width, s->height);
	s->scale = scale;
	corners_create(s);
	wlr_log(WLR_INFO, "output %s: %dx%d, UI %dx%d at scale %d, %s", wo->name,
		wo->width, wo->height, s->width, s->height, scale,
		wo->render_format == DRM_FORMAT_RGB565 ? "16-bit" : "32-bit");

	o = calloc(1, sizeof(*o));
	if (!o)
		return;
	o->server = s;
	o->wlr_output = wo;
	s->output = wo;
	o->frame.notify = output_frame;
	wl_signal_add(&wo->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wo->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wo->events.destroy, &o->destroy);

	lo = wlr_output_layout_add_auto(s->output_layout, wo);
	so = wlr_scene_output_create(s->scene, wo);
	wlr_scene_output_layout_add_output(s->scene_layout, lo, so);
	cursor_fit(s);

	/* Re-place windows created before the output appeared. */
	wl_list_for_each(v, &s->views, link)
		if (v->xdg->base->initialized)
			view_configure(v);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	struct server s = { .width = 320, .height = 240 };
	const char *startup = NULL, *socket;
	int c;

	wlr_log_init(getenv("CHUMBY_WM_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-s startup-command]\n", argv[0]);
			return 1;
		}
	}

	s.display = wl_display_create();
	s.backend = wlr_backend_autocreate(wl_display_get_event_loop(s.display), NULL);
	if (!s.backend) {
		wlr_log(WLR_ERROR, "failed to create backend");
		return 1;
	}
	s.renderer = wlr_renderer_autocreate(s.backend);
	/* A GPU that won't start (EGL failing): wlroots doesn't fall back by
	 * itself where there's a render node, so pixman here. */
	if (!s.renderer) {
		wlr_log(WLR_ERROR, "no GPU renderer: falling back to pixman");
		s.renderer = wlr_pixman_renderer_create();
	}
	if (!s.renderer) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return 1;
	}
	/* The GPU where wlroots has its gles2 renderer and EGL starts (the
	 * Pi), else pixman: the Chumby, or a GPU that wouldn't. */
	wlr_log(WLR_INFO, "renderer: %s", wlr_renderer_is_pixman(s.renderer) ?
		"pixman (software)" : "GPU");
	wlr_renderer_init_wl_display(s.renderer, s.display);
	s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);
	if (!s.allocator) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return 1;
	}

	wlr_compositor_create(s.display, 5, s.renderer);
	wlr_subcompositor_create(s.display);
	wlr_data_device_manager_create(s.display);
	s.ftl_manager = wlr_foreign_toplevel_manager_v1_create(s.display);
	/* Screenshots (chumby-screenshot) */
	wlr_screencopy_manager_v1_create(s.display);

	s.output_layout = wlr_output_layout_create(s.display);
	s.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(s.cursor, s.output_layout);
	s.cursor_idle = wl_event_loop_add_timer(wl_display_get_event_loop(s.display),
					       cursor_idle_cb, &s);
	wlr_xdg_output_manager_v1_create(s.display, s.output_layout);
	s.new_output.notify = new_output;
	wl_signal_add(&s.backend->events.new_output, &s.new_output);

	s.scene = wlr_scene_create();
	s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.output_layout);
	/* Black around the UI (on a bigger screen), dark under it. */
	wlr_scene_rect_create(&s.scene->tree, 4096, 4096, (float[4]){ 0, 0, 0, 1.0f });
	s.ui_tree = wlr_scene_tree_create(&s.scene->tree);
	s.ui_backdrop = wlr_scene_rect_create(s.ui_tree, UI_W, UI_H,
					      (float[4]){ 0.06f, 0.08f, 0.09f, 1.0f });
	s.home_tree = wlr_scene_tree_create(s.ui_tree);
	s.apps_tree = wlr_scene_tree_create(s.ui_tree);
	s.veil_tree = wlr_scene_tree_create(s.ui_tree);
	s.gpu = !wlr_renderer_is_pixman(s.renderer);
	if (s.gpu)
		s.veil_blur = wlr_scene_buffer_create(s.veil_tree, NULL);
	s.veil_dim = wlr_scene_rect_create(s.veil_tree, UI_W, UI_H - BAR_H,
					   (float[4]){ 0, 0, 0, s.gpu ? DIM_GPU : DIM_SOFT });
	wlr_scene_node_set_enabled(&s.veil_tree->node, false);
	s.blur_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s.display),
					       blur_timer_cb, &s);
	s.popup_tree = wlr_scene_tree_create(s.ui_tree);
	s.panel_tree = wlr_scene_tree_create(s.ui_tree);
	s.saver_tree = wlr_scene_tree_create(s.ui_tree);
	s.cal_tree = wlr_scene_tree_create(s.ui_tree);
	s.idle_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s.display),
					       idle_timer_cb, &s);
	wl_event_source_timer_update(s.idle_timer, IDLE_MS);
	s.corner_tree = wlr_scene_tree_create(s.ui_tree);

	wl_list_init(&s.views);
	s.xdg_shell = wlr_xdg_shell_create(s.display, 3);
	s.new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&s.xdg_shell->events.new_toplevel, &s.new_xdg_toplevel);
	s.new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&s.xdg_shell->events.new_popup, &s.new_xdg_popup);

	wl_list_init(&s.keyboards);
	s.cursor_motion.notify = cursor_motion;
	wl_signal_add(&s.cursor->events.motion, &s.cursor_motion);
	s.cursor_motion_abs.notify = cursor_motion_abs;
	wl_signal_add(&s.cursor->events.motion_absolute, &s.cursor_motion_abs);
	s.cursor_button.notify = cursor_button;
	wl_signal_add(&s.cursor->events.button, &s.cursor_button);
	s.cursor_axis.notify = cursor_axis;
	wl_signal_add(&s.cursor->events.axis, &s.cursor_axis);
	s.cursor_frame.notify = cursor_frame;
	wl_signal_add(&s.cursor->events.frame, &s.cursor_frame);
	wl_list_init(&s.touch_devs);
	/* Watch for chumby-calibrate's results (written to a temp name and
	 * renamed into place). */
	{
		int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

		mkdir(CAL_DIR, 0755);
		if (fd >= 0 && inotify_add_watch(fd, CAL_DIR, IN_CLOSE_WRITE | IN_MOVED_TO |
						 IN_DELETE) >= 0)
			wl_event_loop_add_fd(wl_display_get_event_loop(s.display), fd,
					     WL_EVENT_READABLE, cal_changed, &s);
	}
	s.new_input.notify = new_input;
	wl_signal_add(&s.backend->events.new_input, &s.new_input);
	s.seat = wlr_seat_create(s.display, "seat0");

	socket = wl_display_add_socket_auto(s.display);
	if (!socket) {
		wlr_backend_destroy(s.backend);
		return 1;
	}
	if (!wlr_backend_start(s.backend)) {
		wlr_backend_destroy(s.backend);
		wl_display_destroy(s.display);
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup && fork() == 0) {
		execl("/bin/sh", "/bin/sh", "-c", startup, (char *)NULL);
		_exit(127);
	}
	wlr_log(WLR_INFO, "chumby-wm running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(s.display);

	wl_display_destroy_clients(s.display);
	wlr_scene_node_destroy(&s.scene->tree.node);
	wlr_allocator_destroy(s.allocator);
	wlr_renderer_destroy(s.renderer);
	wlr_backend_destroy(s.backend);
	wl_display_destroy(s.display);
	return 0;
}
