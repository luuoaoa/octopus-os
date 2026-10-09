// SPDX-License-Identifier: MIT
/*
 * chumby-wm: a minimal wlroots compositor for the Chumby One's 320x240
 * touchscreen. Derived from wlroots' tinywl.
 *
 * Window policy, keyed on the xdg app_id:
 *   "chumby-panel"  the shell's tab bar: across the top, BAR_H tall,
 *                   always on top
 *   "chumby-popup"  the shell's transient controls (volume, brightness):
 *                   POPUP_W x POPUP_H, dropping down from the bar's right
 *                   end, above apps, below the bar
 *   "chumby-menu"   the shell's app menu (its Home button): dropping down
 *                   from the bar's left end, sized by the shell
 *   "chumby-home"   the app list, behind every app (background layer)
 *   "chumby-calibrate"  the touchscreen calibration tool: the whole
 *                   screen, above everything
 *   anything else   an app: maximized into the area under the bar, one
 *                   visible at a time (the most recently activated). The
 *                   others are frozen (SIGSTOP): nothing of theirs shows,
 *                   so they get no CPU until they're brought back
 *                   (SIGCONT), their last frame already on screen. Their
 *                   sound, if any, is another process (radio, chumby-stream).
 *
 * Apps are exported through wlr-foreign-toplevel-management so the shell
 * can list, activate, minimize and close them. Input is touch (plus an
 * optional USB keyboard); the knob and snooze button are read by the shell
 * directly. While the shell has the backlight off (night), touches are
 * dropped so nothing gets pressed blind.
 *
 * Touch calibration: the matrix chumby-calibrate saves in
 * /var/lib/chumby/touch.cal, applied through libinput at start and again
 * whenever the file changes (inotify); without it, libinput's default
 * (the udev rule's LIBINPUT_CALIBRATION_MATRIX).
 */

#define _POSIX_C_SOURCE 200809L
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
#include <libinput.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
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
#define CAL_DIR		"/var/lib/chumby"
#define CAL_NAME	"touch.cal"
#define CAL_FILE	CAL_DIR "/" CAL_NAME
#define MAX_TOUCH	10

enum view_kind { VIEW_UNKNOWN, VIEW_PANEL, VIEW_HOME, VIEW_POPUP, VIEW_MENU, VIEW_CAL,
		 VIEW_APP };

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;

	/* Stacking: home < apps < popup < panel. */
	struct wlr_scene_tree *home_tree, *apps_tree, *popup_tree, *panel_tree, *cal_tree;

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
	int width, height;
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

static void view_configure(struct view *v)
{
	struct server *s = v->server;

	switch (v->kind) {
	case VIEW_PANEL:
		wlr_scene_node_set_position(&v->tree->node, 0, 0);
		wlr_xdg_toplevel_set_size(v->xdg, s->width, BAR_H);
		break;
	case VIEW_POPUP:
		wlr_scene_node_set_position(&v->tree->node, s->width - POPUP_W, BAR_H);
		wlr_xdg_toplevel_set_size(v->xdg, POPUP_W, POPUP_H);
		break;
	case VIEW_CAL:
		wlr_scene_node_set_position(&v->tree->node, 0, 0);
		wlr_xdg_toplevel_set_size(v->xdg, s->width, s->height);
		break;
	case VIEW_MENU:
		/* Its size is the shell's: as tall as its list of apps. */
		wlr_scene_node_set_position(&v->tree->node, 0, BAR_H);
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
	if (!v->xdg->base->initial_commit)
		return;

	id = v->xdg->app_id ? v->xdg->app_id : "";
	if (!strcmp(id, APP_PANEL)) {
		v->kind = VIEW_PANEL;
		wlr_scene_node_reparent(&v->tree->node, s->panel_tree);
	} else if (!strcmp(id, APP_HOME)) {
		v->kind = VIEW_HOME;
		wlr_scene_node_reparent(&v->tree->node, s->home_tree);
	} else if (!strcmp(id, APP_POPUP)) {
		v->kind = VIEW_POPUP;
		wlr_scene_node_reparent(&v->tree->node, s->popup_tree);
	} else if (!strcmp(id, APP_CAL)) {
		v->kind = VIEW_CAL;
		wlr_scene_node_reparent(&v->tree->node, s->cal_tree);
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

static void touch_down(struct wl_listener *l, void *data)
{
	struct touch_dev *t = wl_container_of(l, t, down);
	struct server *s = t->server;
	struct wlr_touch_down_event *ev = data;
	double lx = ev->x * s->width, ly = ev->y * s->height, sx, sy;
	struct wlr_surface *surface = surface_at(s, lx, ly, &sx, &sy);

	/* Not marked active: its motion and up are dropped too. */
	if (!surface || ev->touch_id < 0 || ev->touch_id >= MAX_TOUCH ||
	    screen_dark())
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
				     ev->x * s->width - s->touch[ev->touch_id].ox,
				     ev->y * s->height - s->touch[ev->touch_id].oy);
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
	default:
		break;
	}
	if (s->have_touch)
		caps |= WL_SEAT_CAPABILITY_TOUCH;
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

	(void)data;
	wlr_scene_output_commit(so, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
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

	wlr_output_init_render(wo, s->allocator, s->renderer);
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	mode = wlr_output_preferred_mode(wo);
	if (mode)
		wlr_output_state_set_mode(&state, mode);
	else	/* modeless outputs (headless backend, for testing): the LCD's size */
		wlr_output_state_set_custom_mode(&state, 320, 240, 0);
	wlr_output_commit_state(wo, &state);
	wlr_output_state_finish(&state);

	s->width = wo->width;
	s->height = wo->height;
	wlr_log(WLR_INFO, "output %s: %dx%d", wo->name, s->width, s->height);

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
	if (!s.renderer) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return 1;
	}
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
	wlr_xdg_output_manager_v1_create(s.display, s.output_layout);
	s.new_output.notify = new_output;
	wl_signal_add(&s.backend->events.new_output, &s.new_output);

	s.scene = wlr_scene_create();
	s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.output_layout);
	/* Dark backdrop until the shell draws. */
	wlr_scene_rect_create(&s.scene->tree, 4096, 4096,
			      (float[4]){ 0.06f, 0.08f, 0.09f, 1.0f });
	s.home_tree = wlr_scene_tree_create(&s.scene->tree);
	s.apps_tree = wlr_scene_tree_create(&s.scene->tree);
	s.popup_tree = wlr_scene_tree_create(&s.scene->tree);
	s.panel_tree = wlr_scene_tree_create(&s.scene->tree);
	s.cal_tree = wlr_scene_tree_create(&s.scene->tree);

	wl_list_init(&s.views);
	s.xdg_shell = wlr_xdg_shell_create(s.display, 3);
	s.new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&s.xdg_shell->events.new_toplevel, &s.new_xdg_toplevel);
	s.new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&s.xdg_shell->events.new_popup, &s.new_xdg_popup);

	wl_list_init(&s.keyboards);
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
