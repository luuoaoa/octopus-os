// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-shell: the Chumby One's system UI on chumby-wm.
 *
 *   tab bar (app_id "chumby-panel", across the top, CHUMBY_BAR_H):
 *     the Home button, the app in front and the one before it as tabs
 *     (tap: switch, hold: close), volume and brightness buttons, the time
 *   menu (app_id "chumby-menu", under the Home button, transient): the
 *     installed apps, to start or bring up one
 *   popup (app_id "chumby-popup", under the bar, transient): the volume
 *     or brightness slider, opened from the bar or the knob
 * With no app running there is nothing under the bar but chumby-wm's
 * backdrop: the menu is the way in.
 *
 * Apps come from one ".app" descriptor each in /usr/share/chumby/apps.
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
 * state, talking to us through the run dir (see chumby.h). Held for
 * 4 s it starts the touchscreen calibration (run at start, too, while
 * there is none). With the volume control open, it stops whatever plays,
 * like that control's stop button.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/input.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include <chumby.h>
#include <lvgl/src/misc/lv_text_private.h>	/* lv_text_get_width(): the menu's names */
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#define APPS_DIR	"/usr/share/chumby/apps"	/* the image's: <name>.app each */
#define BUNDLES_DIR	"/var/apps"	/* installed bundles: <id>/app.conf (chumby-install) */
#define IMAGE_BUNDLES_DIR "/usr/lib/chumby/apps"	/* the image's bundles (read-only) */
#define APP_DATA_DIR	"/var/lib/chumby/apps"	/* a bundle's data: <id>/, the apps' user's */
#define MAX_APPS	16
#define CLOCK_ID	"Clock"		/* app the snooze button brings up */
#define ALARM_TONE	"/usr/bin/chumby-alarm-tone"
#define AUDIO		"/usr/bin/chumby-audio"	/* who plays: see there */
#define CALIBRATE	"/usr/bin/chumby-calibrate"
#define SAVERS		"/usr/lib/chumby/savers"	/* screen savers: policy's "saver" */
#define SAVER_IDLE_MS	(5 * 60 * 1000)	/* no input this long (as chumby-wm's) */
#define CAL_FILE	"/var/lib/chumby/touch.cal"	/* its result */
#define RING_MAX_S	(10 * 60)	/* give up ringing after this */

/* ------------------------------------------------------------------ */
/* Installed apps                                                     */

struct app_info {
	char id[32];		/* xdg app_id the app uses (= its window title) */
	char name[32];
	char short_name[16];	/* for its tab, if the name is long */
	uint32_t color;		/* its hue (Color = #rrggbb), by day */
	char exec[128];
	const char *icon;	/* LV_SYMBOL_* */
	int starting_pid;	/* launched, no window yet (0 = none) */
	time_t starting_since;
	int pid;		/* running (0 = not, as far as we know) */
	char dir[96];		/* a bundle's directory ("": the image's app) */
};

/* An app takes seconds to map its window on this CPU: don't start it again
 * meanwhile (every tap or snooze press would, filling the 64 MB). */
#define LAUNCH_TIMEOUT_S	20

/* The kernel's OOM killer, if it comes to that, ends the oldest app in the
 * background first and the session (-900, chumby-session.service) never:
 * oom_score_adj per app, kept up to date as apps come to the front. */
#define OOM_KEEP	(-500)	/* the alarm tone */
#define OOM_FRONT	0
#define OOM_SAVER	600
#define OOM_BACK	300	/* the latest in the background; +100 each further back */
#define OOM_CLOCK	100	/* the clock in the background: we never evict it */

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
	/* libchumby's own (Material Icons): see CHUMBY_ICON_* */
	{ "radio", CHUMBY_ICON_RADIO }, { "clock", CHUMBY_ICON_CLOCK },
	{ "podcast", CHUMBY_ICON_PODCAST }, { "airplay", CHUMBY_ICON_AIRPLAY },
	{ "globe", CHUMBY_ICON_GLOBE }, { "sun", CHUMBY_ICON_SUN },
};

static const char *icon_symbol(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(icons) / sizeof(icons[0]); i++)
		if (!strcmp(icons[i].name, name))
			return icons[i].sym;
	return LV_SYMBOL_FILE;
}

/* .app files: "key = value" lines with Id, Name, Exec, Icon, Short (a
 * name for its tab, when Name is too long for one) and Color (#rrggbb,
 * its hue: its tab, its active things). */
/* One descriptor into *a: "Key = value" lines (Id, Name, Short, Color,
 * Exec, Icon). A bundle's (dir set) Exec is relative to its directory. */
static bool load_app(const char *path, const char *dir, struct app_info *a)
{
	char line[256], key[32], val[200];
	FILE *f = fopen(path, "r");

	if (!f)
		return false;
	memset(a, 0, sizeof(*a));
	a->icon = LV_SYMBOL_FILE;
	a->color = CHUMBY_ACCENT_DEFAULT;
	if (dir)
		snprintf(a->dir, sizeof(a->dir), "%s", dir);
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, " %31[^= ] = %199[^\n]", key, val) != 2)
			continue;
		if (!strcmp(key, "Id"))
			snprintf(a->id, sizeof(a->id), "%.31s", val);
		else if (!strcmp(key, "Name"))
			snprintf(a->name, sizeof(a->name), "%.31s", val);
		else if (!strcmp(key, "Short"))
			snprintf(a->short_name, sizeof(a->short_name), "%.15s", val);
		else if (!strcmp(key, "Color") && val[0] == '#')
			a->color = (uint32_t)strtoul(val + 1, NULL, 16);
		else if (!strcmp(key, "Exec") && (val[0] == '/' || !dir))
			snprintf(a->exec, sizeof(a->exec), "%.127s", val);
		else if (!strcmp(key, "Exec") && !strstr(val, ".."))
			snprintf(a->exec, sizeof(a->exec), "%.90s/%.36s", dir, val);
		else if (!strcmp(key, "Icon"))
			a->icon = icon_symbol(val);
	}
	fclose(f);
	if (!a->id[0] || !a->exec[0])
		return false;
	if (!a->name[0])
		snprintf(a->name, sizeof(a->name), "%s", a->id);
	return true;
}

/* Every bundle in dir (<id>/app.conf each) not listed yet (by Id). */
static void load_bundles(const char *bundles)
{
	char path[300], dir[96];
	struct dirent *e;
	DIR *d = opendir(bundles);
	int i;

	while (d && (e = readdir(d)) && n_apps < MAX_APPS) {
		if (e->d_name[0] == '.' || strlen(e->d_name) > 40)
			continue;
		snprintf(dir, sizeof(dir), "%s/%s", bundles, e->d_name);
		snprintf(path, sizeof(path), "%s/app.conf", dir);
		if (!load_app(path, dir, &apps[n_apps]))
			continue;
		for (i = 0; i < n_apps && strcmp(apps[i].id, apps[n_apps].id); i++)
			;
		if (i == n_apps)
			n_apps++;
	}
	if (d)
		closedir(d);
}

/* The image's system apps; the installed bundles; the image's bundles
 * (Internet Radio, Podcasts: apps like any, but for being preinstalled).
 * One per Id, the first found: a system app's Id can't be taken, and an
 * installed bundle replaces the image's of its Id (an update). What's
 * running stays known across a reload (an install, a removal). */
static void load_apps(void)
{
	static struct app_info old[MAX_APPS];
	int n_old = n_apps, i, j;
	char path[300];
	struct dirent *e;
	DIR *d;

	memcpy(old, apps, sizeof(apps[0]) * n_old);
	n_apps = 0;
	if ((d = opendir(APPS_DIR))) {
		while ((e = readdir(d)) && n_apps < MAX_APPS) {
			size_t len = strlen(e->d_name);

			if (len < 5 || strcmp(e->d_name + len - 4, ".app"))
				continue;
			snprintf(path, sizeof(path), APPS_DIR "/%s", e->d_name);
			if (load_app(path, NULL, &apps[n_apps]))
				n_apps++;
		}
		closedir(d);
	}
	load_bundles(BUNDLES_DIR);
	load_bundles(IMAGE_BUNDLES_DIR);
	for (i = 0; i < n_apps; i++)
		for (j = 0; j < n_old; j++)
			if (!strcmp(apps[i].id, old[j].id)) {
				apps[i].pid = old[j].pid;
				apps[i].starting_pid = old[j].starting_pid;
				apps[i].starting_since = old[j].starting_since;
			}
}

/* A bundle installed or removed (chumby-install): the list again. */
static void bundles_readable(int fd, void *user)
{
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));

	(void)user;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	load_apps();
	fprintf(stderr, "chumby-shell: apps: %d\n", n_apps);
}

static void bundles_watch(void)
{
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	if (fd < 0 || inotify_add_watch(fd, BUNDLES_DIR, IN_CREATE | IN_DELETE |
					IN_MOVED_TO | IN_MOVED_FROM) < 0) {
		if (fd >= 0)
			close(fd);
		return;		/* (no /var/apps: no bundles) */
	}
	chumby_watch_fd(fd, bundles_readable, NULL);
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
	unsigned long seen;	/* when last in front (0: never) */
	struct toplevel *next;
};

static struct wl_display *wl;
static struct wl_seat *seat;
static struct zwlr_foreign_toplevel_manager_v1 *ftl_manager;
static struct toplevel *toplevels;

static unsigned long seen_seq;

static void tabs_rebuild(void);
static void oom_update(void);

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
	if (t->activated && !t->minimized)
		t->seen = ++seen_seq;
	tabs_rebuild();
	oom_update();
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
	tabs_rebuild();
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

/* ------------------------------------------------------------------ */
/* Apps run as "chumby", not root                                     */

/*
 * The compositor and we are the system UI, as root (the display, every
 * input device, the backlight, the mixer). Everything we start - apps, the
 * screen saver, the alarm tone, chumby-audio - runs as the "chumby" user
 * (groups audio and input, from /etc/group), with HOME its state directory
 * /var/lib/chumby. Not a sandbox: one user for all of them, so they can
 * still stop each other's players (chumby-audio); bundles of others' apps
 * would get users of their own.
 */
#define APP_USER	"chumby"
#define APP_HOME	"/var/lib/chumby"
/* chumby-stream runs above the UI (nice -10): allowed through RLIMIT_NICE
 * (20 - 30 = -10), which root can grant and the user can't. */
#define APP_NICE_LIMIT	30

static uid_t app_uid;
static gid_t app_gid;
static bool app_user_ok;

static void app_user_init(void)
{
	struct passwd *pw = getpwnam(APP_USER);

	if (!pw) {
		fprintf(stderr, "chumby-shell: no user \"" APP_USER "\": apps run as root\n");
		return;
	}
	app_uid = pw->pw_uid;
	app_gid = pw->pw_gid;
	app_user_ok = true;
}

/* path as the apps' user, the kernel's OOM killer told how readily to end
 * it (oom_score_adj, -1000..1000: set before the user drops, so it may be
 * low). -1 if it couldn't start. */
static int spawn_app_in(const struct app_info *app, const char *path, char *const argv[],
			int oom_adj)
{
	struct rlimit nice = { APP_NICE_LIMIT, APP_NICE_LIMIT };
	char adj[16];
	pid_t pid = fork();
	int fd, n;

	if (pid != 0)
		return pid;
	/* The child: nothing of ours but what exec keeps. */
	signal(SIGPIPE, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	if ((fd = open("/proc/self/oom_score_adj", O_WRONLY)) >= 0) {
		n = snprintf(adj, sizeof(adj), "%d", oom_adj);
		if (write(fd, adj, n) < 0)
			;
		close(fd);
	}
	setrlimit(RLIMIT_NICE, &nice);
	if (app_user_ok) {
		if (initgroups(APP_USER, app_gid) < 0 || setgid(app_gid) < 0 ||
		    setuid(app_uid) < 0)
			_exit(127);
		setenv("HOME", APP_HOME, 1);
		setenv("USER", APP_USER, 1);
	}
	/* A bundle: where it is (read-only), and a directory of its own for
	 * its data (libchumby: chumby_app_dir(), chumby_app_data_dir()). */
	if (app && app->dir[0]) {
		char data[96];

		snprintf(data, sizeof(data), APP_DATA_DIR "/%s", app->id);
		setenv("CHUMBY_APP_DIR", app->dir, 1);
		setenv("CHUMBY_APP_DATA", data, 1);
	}
	execv(path, argv);
	_exit(127);
}

static int spawn_app(const char *path, char *const argv[], int oom_adj)
{
	return spawn_app_in(NULL, path, argv, oom_adj);
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
	if (a->dir[0]) {	/* a bundle's data directory, the apps' user's */
		char data[96];

		mkdir(APP_DATA_DIR, 0755);
		snprintf(data, sizeof(data), APP_DATA_DIR "/%s", a->id);
		mkdir(data, 0755);
		if (app_user_ok && (chown(APP_DATA_DIR, app_uid, app_gid) < 0 ||
				    chown(data, app_uid, app_gid) < 0))
			fprintf(stderr, "chumby-shell: %s: %s\n", data, strerror(errno));
	}
	pid = spawn_app_in(a, a->exec, argv, OOM_FRONT);
	if (pid < 0) {
		fprintf(stderr, "chumby-shell: cannot start %s\n", a->exec);
		return;
	}
	a->starting_pid = a->pid = pid;
	a->starting_since = time(NULL);
}

/* ------------------------------------------------------------------ */
/* Tab bar ("chumby-panel", across the top)                           */

/*
 * Apps are tabs, Palm OS style: the app in front (filled, icon and name)
 * and the one before it (icon only), standing on a line that runs the
 * width of the screen and sets the shell apart from the app. Tap a tab to
 * switch, hold one to close its app.
 *
 * Before them, the Home button: like Windows' Start button it isn't a
 * place but a menu. A plain icon, it opens the list of apps as a card
 * just under the bar; picking one starts it or brings it up. The app in
 * front stays until then.
 *
 * At the right end: volume, brightness and the time. Volume and
 * brightness drop their slider down from the bar, under them; their
 * button fills like a tab in the text color (set apart from the accent
 * app tab) and joins the slider's plate, so it's plain which one is being
 * set.
 *
 * Tabs are drawn taller than the bar: the window clips their bottom
 * (rounded) corners, leaving square feet on the line.
 */
#define TAB_Y		5	/* a tab's top, filled or not */
#define TAB_FOOT	10	/* drawn below the bar, clipped */
#define LINE_H		2
#define TAB_GAP		3
#define CORNER_R	8	/* every corner: the screen's (chumby-wm), tabs, plates */
#define TAB_TITLE_MAX	80
#define POPUP_IDLE_MS	4000
#define STOP_SIZE	36	/* the volume control's stop button */
/* The menu card and the sliders' plate, a little see-through: under them
 * chumby-wm dims the app (and blurs it, on a GPU). */
#define POPUP_OPA	LV_OPA_80
static void stop_btn_cb(lv_event_t *e);

static lv_obj_t *panel_scr;
static lv_obj_t *btn_home, *btn_vol, *btn_bri, *bar_clock;
/* The clock's own tab, at the far right: the time, or "Clock" while the
 * clock app is in front. */
static lv_obj_t *bar_clock_tab;
static bool clock_front;
static int volume_pct = 50;	/* cached: the mixer may not exist yet */

enum popup_kind { POPUP_NONE, POPUP_VOLUME, POPUP_BRIGHTNESS };
static enum popup_kind popup_kind;
static lv_display_t *popup_disp;
static lv_obj_t *popup_bar;
static lv_obj_t *popup_stop;	/* volume: stop what plays (if anything does) */
static bool media_playing;	/* chumby-audio's $run_dir/audio/playing is there */
static lv_timer_t *popup_timer;

/* The app in front: activated and not minimized (none: Home is). */
static struct toplevel *tl_current(void)
{
	struct toplevel *t;

	for (t = toplevels; t; t = t->next)
		if (t->activated && !t->minimized)
			return t;
	return NULL;
}

static bool is_clock(const char *app_id)
{
	return app_id && !strcmp(app_id, CLOCK_ID);
}

/* The app that was in front last, other than `cur` and `skip` (any
 * other, if none was); never the clock, which has its own tab. */
static struct toplevel *tl_previous(struct toplevel *cur, struct toplevel *skip)
{
	struct toplevel *t, *best = NULL;

	for (t = toplevels; t; t = t->next)
		if (t != cur && t != skip && !is_clock(t->app_id) &&
		    (!best || t->seen > best->seen))
			best = t;
	return best;
}

static bool tl_alive(struct toplevel *t)
{
	struct toplevel *i;

	for (i = toplevels; i && i != t; i = i->next)
		;
	return i != NULL;
}

/* Filled or not, a tab keeps its size (the border stays, unseen when
 * filled): switching never nudges its neighbours. */
static void tab_style(lv_obj_t *b, bool on)
{
	lv_obj_set_style_radius(b, CORNER_R, 0);
	lv_obj_set_style_bg_color(b, chumby_color(on ? CHUMBY_COLOR_ACCENT
						     : CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(b, 1, 0);
	lv_obj_set_style_border_color(b, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_border_opa(b, on ? LV_OPA_TRANSP : LV_OPA_40, 0);
	lv_obj_set_style_text_color(b, chumby_color(on ? CHUMBY_COLOR_BG
						       : CHUMBY_COLOR_TEXT_DIM), 0);
}

/* A box standing on the line (bottom-aligned in its row), its content
 * centred in the part above the line. */
static lv_obj_t *bar_box(lv_obj_t *parent)
{
	lv_obj_t *b = lv_obj_create(parent);

	lv_obj_remove_style_all(b);
	lv_obj_set_scrollable(b, false);
	lv_obj_set_size(b, LV_SIZE_CONTENT, CHUMBY_BAR_H - TAB_Y + TAB_FOOT);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_bottom(b, TAB_FOOT + LINE_H, 0);
	return b;
}

static lv_obj_t *tab_create(lv_obj_t *parent, const char *sym, const char *name, bool on)
{
	lv_obj_t *b = bar_box(parent), *l;

	lv_obj_set_clickable(b, true);
	lv_obj_set_style_min_width(b, 36, 0);
	lv_obj_set_style_pad_hor(b, name ? 9 : 10, 0);
	lv_obj_set_style_pad_column(b, 5, 0);
	/* Up to the screen's top edge, and the gaps between tabs. */
	lv_obj_set_ext_click_area(b, TAB_Y);
	tab_style(b, on);

	l = lv_label_create(b);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, chumby_icon_font(14), 0);
	if (name) {
		l = lv_label_create(b);
		lv_label_set_text(l, name);
		lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_BODY), 0);
		/* Room for the tabs and the tray, whatever the name: one line,
		 * cut with dots (a fixed width; max-width alone wraps). */
		lv_obj_update_layout(l);
		if (lv_obj_get_width(l) > TAB_TITLE_MAX) {
			lv_obj_set_width(l, TAB_TITLE_MAX);
			lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
		}
	}
	return b;
}

static void tray_style(lv_obj_t *b, bool on)
{
	tab_style(b, on);
	if (on) {	/* not the accent: that's the app's tab */
		lv_obj_set_style_bg_color(b, chumby_color(CHUMBY_COLOR_TEXT), 0);
		/* As see-through as the plate it joins. */
		lv_obj_set_style_bg_opa(b, POPUP_OPA, 0);
	}
	else	/* plain icon at rest */
		lv_obj_set_style_border_opa(b, LV_OPA_TRANSP, 0);
}

static lv_obj_t *tray_button(lv_obj_t *parent, const char *sym)
{
	lv_obj_t *b = tab_create(parent, sym, NULL, false);

	/* Narrower than a tab: the two sit close together. */
	lv_obj_set_style_min_width(b, 26, 0);
	lv_obj_set_style_pad_hor(b, 4, 0);
	return b;
}

static lv_obj_t *bar_tabs;	/* the tab row */
static lv_obj_t *bar_line;	/* the line under them */
static void popup_close(void);

/* The app menu ("chumby-menu"): a row per installed app. */
#define MENU_W		184
#define MENU_W_WIDE	(CHUMBY_SCREEN_W - 8)	/* two columns: more apps than rows fit */
#define MENU_ROWS	4	/* rows that fit under the bar */
#define MENU_ROW_H	44
#define MENU_PAD	6
#define MENU_IDLE_MS	8000
#define MENU_RADIUS	CORNER_R

static lv_display_t *menu_disp;
static lv_timer_t *menu_timer;

/*
 * An open button (Home, volume, brightness) and the plate it drops down
 * are one shape: its fill runs through the line, and concave rounded
 * corners on either side of its foot curve out into the plate's top edge.
 */
#define JOIN_R		6
static lv_obj_t *bar_join;	/* that band and its two fillets */

static lv_obj_t *join_part(int x, int y, int w, int h)
{
	lv_obj_t *o = lv_obj_create(bar_join);

	lv_obj_remove_style_all(o);
	lv_obj_set_clickable(o, false);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(o, chumby_color(CHUMBY_COLOR_TEXT), 0);
	return o;
}

/* A fillet: a square of the fill with a circle of the bar's background
 * over it, centred on its outer top corner (and clipped to the square). */
static void join_fillet(int x, int right, int r)
{
	int y = CHUMBY_BAR_H - LINE_H - r;
	lv_obj_t *sq = join_part(right ? x : x - r, y, r, r);
	lv_obj_t *c = lv_obj_create(sq);

	lv_obj_remove_style_all(c);
	lv_obj_set_clickable(c, false);
	lv_obj_set_size(c, 2 * r, 2 * r);
	lv_obj_set_pos(c, right ? 0 : -r, -r);
	lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(c, chumby_color(CHUMBY_COLOR_BAR), 0);
}

static void bar_join_update(void)
{
	lv_obj_t *b = popup_kind == POPUP_VOLUME ? btn_vol :
		      popup_kind == POPUP_BRIGHTNESS ? btn_bri : NULL;
	lv_obj_t *next;
	lv_area_t a;
	int r_left = JOIN_R, r_right = JOIN_R;

	if (!bar_join)
		return;
	lv_obj_clean(bar_join);
	if (!b)
		return;
	lv_obj_update_layout(b);
	lv_obj_get_coords(b, &a);
	if (a.x1 <= 0)		/* at the screen's edge (Home): none there */
		r_left = 0;
	/* Not into a filled tab beside it: only as wide as the gap. */
	next = lv_obj_get_sibling(b, 1);
	if ((b == btn_home && next) || (next == bar_clock_tab && clock_front))
		r_right = TAB_GAP;
	if (b == btn_vol || b == btn_bri)	/* the plate's right edge is its */
		r_right = 0;
	join_part(a.x1 - r_left, CHUMBY_BAR_H - LINE_H,
		  lv_area_get_width(&a) + r_left + r_right, LINE_H);
	if (r_left)
		join_fillet(a.x1, 0, r_left);
	if (r_right)
		join_fillet(a.x2 + 1, 1, r_right);
}

/* A plain icon, menu open or not: not a tab, no border, no fill (its
 * menu is a card of its own under the bar). */
static void home_style(void)
{
	if (!btn_home)
		return;
	tab_style(btn_home, false);
	lv_obj_set_style_border_opa(btn_home, LV_OPA_TRANSP, 0);
}

static void menu_close(void)
{
	if (!menu_disp)
		return;
	chumby_window_close(menu_disp);
	menu_disp = NULL;
	lv_timer_pause(menu_timer);
	home_style();
}

static void menu_close_async(void *user)
{
	(void)user;
	menu_close();
}

static void menu_timer_cb(lv_timer_t *t)
{
	(void)t;
	menu_close();
}


static void menu_row_cb(lv_event_t *e)
{
	launch(lv_event_get_user_data(e));
	/* Not from inside its own window's event. */
	lv_async_call(menu_close_async, NULL);
}

static void menu_open(void)
{
	lv_obj_t *scr, *plate, *row, *l;
	int i, n = 0, h, w, cols, rows, item_w;

	for (i = 0; i < n_apps; i++)
		n += !is_clock(apps[i].id);
	/* One column while they fit under the bar (MENU_ROWS: 200 px); then
	 * two, nearly the screen's width; past MENU_ROWS rows of those, it
	 * scrolls: three rows and half of the next, which says so. */
	cols = n > MENU_ROWS ? 2 : 1;
	w = cols == 2 ? MENU_W_WIDE : MENU_W;
	rows = n ? (n + cols - 1) / cols : 1;
	if (rows <= MENU_ROWS)
		h = rows * (MENU_ROW_H + 4) - 4 + 2 * MENU_PAD;
	else
		h = (MENU_ROWS - 1) * (MENU_ROW_H + 4) + MENU_ROW_H / 2 + MENU_PAD;
	item_w = (w - 2 * MENU_PAD - (cols - 1) * 4) / cols;

	popup_close();
	if (menu_disp)
		return;
	/* With alpha: the plate's rounded corners show what's under them. */
	lv_wayland_window_set_next_color_format(LV_COLOR_FORMAT_ARGB8888);
	menu_disp = chumby_window_create("chumby-menu", w, h);
	if (!menu_disp)
		return;
	lv_timer_reset(menu_timer);
	lv_timer_resume(menu_timer);
	scr = lv_display_get_screen_active(menu_disp);
	lv_obj_set_scrollable(scr, false);
	lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);

	/* A card of its own, rounded all round, in the highlight shade. */
	plate = lv_obj_create(scr);
	lv_obj_remove_style_all(plate);
	lv_obj_set_size(plate, w, h);
	lv_obj_set_scrollable(plate, rows > MENU_ROWS);
	lv_obj_set_scroll_dir(plate, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(plate, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_radius(plate, MENU_RADIUS, 0);
	lv_obj_set_style_bg_opa(plate, POPUP_OPA, 0);
	lv_obj_set_style_bg_color(plate, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_flex_flow(plate, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_all(plate, MENU_PAD, 0);
	lv_obj_set_style_pad_row(plate, 4, 0);
	lv_obj_set_style_pad_column(plate, 4, 0);

	for (i = 0; i < n_apps; i++) {
		if (is_clock(apps[i].id))
			continue;
		row = lv_button_create(plate);
		lv_obj_set_size(row, item_w, MENU_ROW_H);
		lv_obj_set_style_radius(row, 8, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(row, chumby_color(CHUMBY_COLOR_BG), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_PRESSED);
		lv_obj_set_style_text_color(row, chumby_color(CHUMBY_COLOR_BG), 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_left(row, 10, 0);
		lv_obj_set_style_pad_column(row, 12, 0);
		lv_obj_add_event_cb(row, menu_row_cb, LV_EVENT_CLICKED, &apps[i]);

		l = lv_label_create(row);
		lv_label_set_text(l, apps[i].icon);
		lv_obj_set_style_text_font(l, chumby_icon_font(20), 0);
		lv_obj_set_style_text_color(l, chumby_accent(apps[i].color), 0);
		lv_obj_set_width(l, 24);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		l = lv_label_create(row);
		lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_LARGE), 0);
		{
			/* Too long for its column: the tab's name (Short), and
			 * anything longer still on one line, cut with dots. */
			const lv_font_t *f = chumby_font(CHUMBY_FONT_LARGE);
			const char *name = apps[i].name;
			int room = item_w - 10 - 24 - 12 - 8;	/* pads, icon, gap */
			/* (A struct, not a letter spacing: 0 there was NULL.) */
			lv_text_attributes_t attr = { .max_width = LV_COORD_MAX };

			if (apps[i].short_name[0] &&
			    lv_text_get_width(name, strlen(name), f, &attr) > room)
				name = apps[i].short_name;
			lv_label_set_text(l, name);
			lv_obj_set_flex_grow(l, 1);
			lv_obj_set_height(l, lv_font_get_line_height(f));
			lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
		}
	}
	if (!n) {
		l = lv_label_create(plate);
		lv_obj_set_style_text_color(l, chumby_color(CHUMBY_COLOR_BG), 0);
		lv_label_set_text(l, "No apps installed");
	}
	home_style();
}

static void home_tab_cb(lv_event_t *e)
{
	(void)e;
	if (menu_disp)
		menu_close();
	else
		menu_open();
}

static void app_tab_cb(lv_event_t *e)
{
	struct toplevel *t = lv_event_get_user_data(e);

	menu_close();
	popup_close();
	if (!tl_alive(t))	/* closed since the tab was built */
		return;
	if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
		zwlr_foreign_toplevel_handle_v1_close(t->handle);
	else
		activate(t);
	wl_display_flush(wl);
}

static void bar_clock_update(void);

/* The clock's tab: filled, "Clock", while its app is in front; else just
 * the time, no tab. Its width stays put (the tray doesn't shift). */
static void clock_tab_style(void)
{
	struct app_info *a = app_by_id(CLOCK_ID);

	if (!bar_clock_tab)
		return;
	tab_style(bar_clock_tab, clock_front);
	if (clock_front) {
		chumby_glossy(bar_clock_tab, chumby_accent(a ? a->color : CHUMBY_ACCENT_DEFAULT), 0);
		lv_obj_set_style_text_color(bar_clock_tab, chumby_on_accent(), 0);
		lv_obj_set_style_text_font(bar_clock, chumby_font(CHUMBY_FONT_BODY), 0);
		/* On the app tabs' title baseline (their row is centred with
		 * the icon font's taller line in it). */
		lv_obj_set_style_translate_y(bar_clock, -1, 0);
		lv_obj_set_style_text_color(bar_clock, chumby_on_accent(), 0);
		lv_label_set_text(bar_clock, "Clock");
	} else {
		lv_obj_set_style_border_opa(bar_clock_tab, LV_OPA_TRANSP, 0);
		/* Bigger than the tab titles, but on their baseline (centred,
		 * the 19 px font's sits a pixel lower). */
		lv_obj_set_style_text_font(bar_clock, chumby_font(CHUMBY_FONT_LARGE), 0);
		lv_obj_set_style_translate_y(bar_clock, -1, 0);
		lv_obj_set_style_text_color(bar_clock, chumby_color(CHUMBY_COLOR_TEXT), 0);
		lv_label_set_text(bar_clock, "");
		bar_clock_update();
	}
}

static void show_clock(void);

static void clock_tab_cb(lv_event_t *e)
{
	(void)e;
	if (!clock_front)
		show_clock();
}

/* An app not in front: its icon, in its hue (dim at night). */
static void prev_tab(struct toplevel *t)
{
	struct app_info *a;
	lv_obj_t *b;

	if (!t)
		return;
	a = app_by_id(t->app_id);
	b = tab_create(bar_tabs, a ? a->icon : LV_SYMBOL_FILE, NULL, false);
	if (a && chumby_mode() == CHUMBY_MODE_DAY)
		lv_obj_set_style_text_color(lv_obj_get_child(b, 0), chumby_accent(a->color), 0);
	lv_obj_add_event_cb(b, app_tab_cb, LV_EVENT_SHORT_CLICKED, t);
	lv_obj_add_event_cb(b, app_tab_cb, LV_EVENT_LONG_PRESSED, t);
}

static void tabs_rebuild(void)
{
	struct toplevel *cur, *prev, *prev2 = NULL;
	struct app_info *a;
	lv_obj_t *b;

	if (!bar_tabs)
		return;
	lv_obj_clean(bar_tabs);
	cur = tl_current();
	clock_front = cur && is_clock(cur->app_id);
	/* Two app tabs beside Home, the clock not counted: the app in front
	 * and the one before it, or (the clock in front) the last two. */
	prev = tl_previous(cur, NULL);
	if (clock_front)
		prev2 = tl_previous(cur, prev);
	clock_tab_style();

	/* Home: a button for the menu, filled only while that is open. */
	btn_home = tab_create(bar_tabs, LV_SYMBOL_HOME, NULL, menu_disp != NULL);
	lv_obj_set_style_pad_hor(btn_home, 13, 0);	/* a little roomier */
	lv_obj_add_event_cb(btn_home, home_tab_cb, LV_EVENT_CLICKED, NULL);
	home_style();
	if (cur && !clock_front) {	/* the clock's tab is at the right */
		a = app_by_id(cur->app_id);
		b = tab_create(bar_tabs, a ? a->icon : LV_SYMBOL_FILE,
			       !a ? cur->app_id : a->short_name[0] ? a->short_name : a->name, true);
		chumby_glossy(b, chumby_accent(a ? a->color : CHUMBY_ACCENT_DEFAULT), 0);
		lv_obj_set_style_text_color(b, chumby_on_accent(), 0);
		lv_obj_add_event_cb(b, app_tab_cb, LV_EVENT_LONG_PRESSED, cur);
	}
	/* The line the tabs stand on: the app in front's hue. */
	if (bar_line)
		lv_obj_set_style_bg_color(bar_line, chumby_accent(cur && app_by_id(cur->app_id) ?
					  app_by_id(cur->app_id)->color : CHUMBY_ACCENT_DEFAULT), 0);
	prev_tab(prev);
	prev_tab(prev2);
}

static void bar_clock_update(void)
{
	char buf[8] = "";
	time_t now = time(NULL);
	struct tm tm;

	if (!bar_clock || clock_front)	/* "Clock" */
		return;
	if (chumby_time_valid()) {
		localtime_r(&now, &tm);
		strftime(buf, sizeof(buf), "%H:%M", &tm);
	}
	if (strcmp(lv_label_get_text(bar_clock), buf))
		lv_label_set_text(bar_clock, buf);
}

static void bar_clock_cb(lv_timer_t *t)
{
	(void)t;
	bar_clock_update();
}

/* ------------------------------------------------------------------ */
/* Volume / brightness slider ("chumby-popup", under the bar)        */

static void quick_buttons_update(void)
{
	if (btn_vol)
		tray_style(btn_vol, popup_kind == POPUP_VOLUME);
	if (btn_bri)
		tray_style(btn_bri, popup_kind == POPUP_BRIGHTNESS);
	bar_join_update();
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

/* The brightness, remembered per mode: set in the popup, it's the mode's
 * from then on (the policy's day_/night_brightness until first set), kept
 * on the card for the next start. */
#define BRIGHTNESS_FILE	"/var/lib/chumby/brightness"
static int brightness_saved[2] = { -1, -1 };	/* per chumby_mode_t */
static bool brightness_dirty;

static void brightness_load(void)
{
	FILE *f = fopen(BRIGHTNESS_FILE, "r");
	char line[64];
	int v;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "day %d", &v) == 1 && v >= 5 && v <= 100)
			brightness_saved[CHUMBY_MODE_DAY] = v;
		else if (sscanf(line, "night %d", &v) == 1 && v >= 5 && v <= 100)
			brightness_saved[CHUMBY_MODE_NIGHT] = v;
	}
	fclose(f);
}

static void brightness_save(void)
{
	FILE *f;

	if (!brightness_dirty)
		return;
	brightness_dirty = false;
	if (!(f = fopen(BRIGHTNESS_FILE ".new", "w")))
		return;
	if (brightness_saved[CHUMBY_MODE_DAY] >= 0)
		fprintf(f, "day %d\n", brightness_saved[CHUMBY_MODE_DAY]);
	if (brightness_saved[CHUMBY_MODE_NIGHT] >= 0)
		fprintf(f, "night %d\n", brightness_saved[CHUMBY_MODE_NIGHT]);
	if (fclose(f) == 0)
		rename(BRIGHTNESS_FILE ".new", BRIGHTNESS_FILE);
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
		brightness_saved[chumby_mode() == CHUMBY_MODE_NIGHT] = v;
		brightness_dirty = true;	/* written when the popup closes */
	}
	if (popup_bar)
		lv_slider_set_value(popup_bar, v, LV_ANIM_OFF);
	popup_keepalive();
}

static void popup_close(void)
{
	brightness_save();
	if (!popup_disp)
		return;
	chumby_window_close(popup_disp);
	popup_disp = NULL;
	popup_bar = NULL;
	popup_stop = NULL;
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

/*
 * The slider hangs from the bar under its button (chumby-wm places
 * "chumby-popup" there): a plate the same color as the filled button,
 * the bar dark on it. Tap or drag anywhere on it. It closes
 * after POPUP_IDLE_MS or on its button.
 */
/* The bar: beside the stop button while that shows, else the plate's width. */
static void popup_layout(void)
{
	if (!popup_bar)
		return;
	if (popup_stop && !lv_obj_is_hidden(popup_stop)) {
		lv_obj_set_size(popup_bar, CHUMBY_POPUP_W - 8 - STOP_SIZE - 10 - 16,
				CHUMBY_POPUP_H - 2 * 14);
		lv_obj_align(popup_bar, LV_ALIGN_LEFT_MID, 8 + STOP_SIZE + 10, 0);
	} else {
		lv_obj_set_size(popup_bar, CHUMBY_POPUP_W - 2 * 16, CHUMBY_POPUP_H - 2 * 14);
		lv_obj_center(popup_bar);
	}
}

static void popup_stop_show(bool show)
{
	if (!popup_stop)
		return;
	lv_obj_set_hidden(popup_stop, !show);
	popup_layout();
}

/*
 * What plays: chumby-audio keeps $run_dir/audio/playing (the source's
 * name; gone while nothing plays) and we watch that directory, so the
 * stop button is right when the popup opens and follows a player that
 * starts or stops while it's open - nothing to ask, nothing to wait for.
 */
static char playing_path[256];

static void media_playing_read(void)
{
	struct stat st;

	media_playing = stat(playing_path, &st) == 0 && st.st_size > 0;
	popup_stop_show(media_playing);
}

static void audio_dir_readable(int fd, void *user)
{
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));

	(void)user;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	media_playing_read();
}

static void audio_watch(void)
{
	char dir[256];
	int fd;

	snprintf(dir, sizeof(dir), "%s/audio", chumby_run_dir());
	snprintf(playing_path, sizeof(playing_path), "%s/playing", dir);
	mkdir(chumby_run_dir(), 0755);
	mkdir(dir, 0755);	/* chumby-audio makes it too: whoever's first */
	fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0 || inotify_add_watch(fd, dir, IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE |
					IN_DELETE | IN_MOVED_FROM) < 0) {
		fprintf(stderr, "chumby-shell: cannot watch %s\n", dir);
		if (fd >= 0)
			close(fd);
		return;
	}
	chumby_watch_fd(fd, audio_dir_readable, NULL);
	media_playing_read();
}

static void popup_open(enum popup_kind kind)
{
	lv_obj_t *scr, *plate, *sq;

	lv_obj_t *btn = kind == POPUP_VOLUME ? btn_vol : btn_bri;
	char app_id[32];
	lv_area_t a;

	menu_close();
	if (!btn)	/* no brightness control here */
		return;
	if (popup_kind == kind && popup_disp) {
		popup_keepalive();
		return;
	}
	popup_close();	/* the other one: it's under its own button */
	/* Its right edge under its button's (chumby-wm reads it from the
	 * app_id): the screen's edge is a stretch to reach. */
	lv_obj_update_layout(btn);
	lv_obj_get_coords(btn, &a);
	snprintf(app_id, sizeof(app_id), "chumby-popup@%d", (int)a.x2 + 1);
	/* With alpha: the plate's rounded corners show what's under them. */
	lv_wayland_window_set_next_color_format(LV_COLOR_FORMAT_ARGB8888);
	popup_disp = chumby_window_create(app_id, CHUMBY_POPUP_W, CHUMBY_POPUP_H);
	if (!popup_disp)
		return;
	popup_kind = kind;
	scr = lv_display_get_screen_active(popup_disp);
	lv_obj_clean(scr);
	lv_obj_set_scrollable(scr, false);
	lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
	plate = lv_obj_create(scr);
	lv_obj_remove_style_all(plate);
	lv_obj_set_size(plate, CHUMBY_POPUP_W, CHUMBY_POPUP_H);
	lv_obj_set_scrollable(plate, false);
	lv_obj_set_style_radius(plate, MENU_RADIUS, 0);
	lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(plate, chumby_color(CHUMBY_COLOR_TEXT), 0);
	/* See-through as a whole (one layer): the corner square below would
	 * show darker where it overlaps the plate if each were. */
	lv_obj_set_style_opa_layered(plate, POPUP_OPA, 0);
	/* Square where it joins its button (top right). */
	sq = lv_obj_create(plate);
	lv_obj_remove_style_all(sq);
	lv_obj_set_size(sq, MENU_RADIUS, MENU_RADIUS);
	lv_obj_align(sq, LV_ALIGN_TOP_RIGHT, 0, 0);
	lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(sq, chumby_color(CHUMBY_COLOR_TEXT), 0);

	popup_bar = lv_slider_create(plate);
	popup_stop = NULL;
	if (kind == POPUP_VOLUME) {
		/* Volume: a stop button while something plays (audio_watch()),
		 * then the bar. */
		lv_obj_t *stop = lv_button_create(plate), *l;

		popup_stop = stop;
		lv_obj_set_hidden(stop, !media_playing);
		lv_obj_set_size(stop, STOP_SIZE, STOP_SIZE);
		lv_obj_align(stop, LV_ALIGN_LEFT_MID, 8, 0);
		lv_obj_set_style_radius(stop, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(stop, 0, 0);
		lv_obj_set_style_bg_color(stop, chumby_color(CHUMBY_COLOR_BG), 0);
		lv_obj_set_style_bg_opa(stop, LV_OPA_30, 0);
		lv_obj_set_style_bg_opa(stop, LV_OPA_60, LV_STATE_PRESSED);
		lv_obj_set_style_text_color(stop, chumby_color(CHUMBY_COLOR_BG), 0);
		lv_obj_add_event_cb(stop, stop_btn_cb, LV_EVENT_CLICKED, NULL);
		l = lv_label_create(stop);
		lv_label_set_text(l, LV_SYMBOL_STOP);
		lv_obj_center(l);
	}
	popup_layout();
	lv_slider_set_range(popup_bar, kind == POPUP_VOLUME ? 0 : 5, 100);
	lv_obj_set_style_radius(popup_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(popup_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(popup_bar, chumby_color(CHUMBY_COLOR_BG), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(popup_bar, LV_OPA_30, LV_PART_MAIN);
	chumby_glossy(popup_bar, chumby_color(CHUMBY_COLOR_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(popup_bar, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_pad_all(popup_bar, 0, LV_PART_KNOB);
	/* The whole popup height is the touch target. */
	lv_obj_set_ext_click_area(popup_bar, 14);
	lv_obj_add_event_cb(popup_bar, popup_bar_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(popup_bar, popup_bar_cb, LV_EVENT_PRESSING, NULL);

	popup_set(popup_get());
	quick_buttons_update();
}

/* Stop whatever plays (FM, a stream: every chumby-audio source). */
static void media_stop(void)
{
	char *argv[] = { "chumby-audio", "stop", NULL };

	spawn_app(AUDIO, argv, OOM_FRONT);
	if (popup_disp) {
		popup_stop_show(false);	/* nothing plays now */
		popup_keepalive();
	}
}

static void stop_btn_cb(lv_event_t *e)
{
	(void)e;
	media_stop();
}

/* Tray button: open its slider, or close it if it's already showing. */
static void quick_btn_cb(lv_event_t *e)
{
	enum popup_kind kind = (enum popup_kind)(intptr_t)lv_event_get_user_data(e);

	if (popup_kind == kind)
		popup_close();
	else
		popup_open(kind);
}

/* The bar's widgets (again on a day/night switch: colors are per mode). */
static void build_panel(lv_obj_t *scr)
{
	lv_obj_t *tray, *line, *box;
	int w;

	lv_obj_clean(scr);
	lv_obj_set_scrollable(scr, false);
	lv_obj_set_style_bg_color(scr, chumby_color(CHUMBY_COLOR_BAR), 0);

	/* Rows as tall as the tabs, standing on the bar's bottom edge (the
	 * part below it is clipped). */
	bar_tabs = lv_obj_create(scr);
	lv_obj_remove_style_all(bar_tabs);
	lv_obj_set_scrollable(bar_tabs, false);
	lv_obj_set_size(bar_tabs, LV_SIZE_CONTENT, CHUMBY_BAR_H + TAB_FOOT);
	lv_obj_set_pos(bar_tabs, 0, 0);
	lv_obj_set_flex_flow(bar_tabs, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar_tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
			      LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(bar_tabs, TAB_GAP, 0);

	/* The line, over the tabs' feet (it joins the filled tab), under the
	 * tray's (a filled tray button runs through it into its slider). */
	line = lv_obj_create(scr);
	lv_obj_remove_style_all(line);
	lv_obj_set_size(line, CHUMBY_SCREEN_W, LINE_H);
	lv_obj_set_pos(line, 0, CHUMBY_BAR_H - LINE_H);
	lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(line, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	bar_line = line;

	tray = lv_obj_create(scr);
	lv_obj_remove_style_all(tray);
	lv_obj_set_scrollable(tray, false);
	lv_obj_set_size(tray, LV_SIZE_CONTENT, CHUMBY_BAR_H + TAB_FOOT);
	lv_obj_align(tray, LV_ALIGN_TOP_RIGHT, -4, 0);
	lv_obj_set_flex_flow(tray, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tray, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
			      LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(tray, TAB_GAP, 0);

	/* Brightness, then volume: the volume popup is the wider one (its
	 * stop button) and hangs from its button's right edge, so that is
	 * the one further right. */
	btn_bri = NULL;
	if (chumby_brightness_get() >= 0) {	/* none without a backlight we drive */
		btn_bri = tray_button(tray, CHUMBY_ICON_SUN);
		lv_obj_add_event_cb(btn_bri, quick_btn_cb, LV_EVENT_CLICKED,
				    (void *)(intptr_t)POPUP_BRIGHTNESS);
	}
	btn_vol = tray_button(tray, LV_SYMBOL_VOLUME_MAX);
	lv_obj_add_event_cb(btn_vol, quick_btn_cb, LV_EVENT_CLICKED,
			    (void *)(intptr_t)POPUP_VOLUME);
	box = bar_box(tray);
	bar_clock_tab = box;
	lv_obj_set_clickable(box, true);
	lv_obj_set_ext_click_area(box, TAB_Y);
	lv_obj_add_event_cb(box, clock_tab_cb, LV_EVENT_CLICKED, NULL);
	bar_clock = lv_label_create(box);
	/* As wide as the wider of the time and the "Clock" tab. */
	lv_label_set_text(bar_clock, "Clock");
	lv_obj_set_style_text_font(bar_clock, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_update_layout(bar_clock);
	w = lv_obj_get_width(bar_clock) + 2 * 9;
	lv_label_set_text(bar_clock, "00:00");
	lv_obj_set_style_text_font(bar_clock, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_update_layout(bar_clock);
	if (lv_obj_get_width(bar_clock) + 2 * 4 > w)
		w = lv_obj_get_width(bar_clock) + 2 * 4;
	lv_obj_set_width(box, w);
	lv_label_set_text(bar_clock, "");

	/* Over everything else in the bar. */
	bar_join = lv_obj_create(scr);
	lv_obj_remove_style_all(bar_join);
	lv_obj_set_clickable(bar_join, false);
	lv_obj_set_size(bar_join, CHUMBY_SCREEN_W, CHUMBY_BAR_H);

	clock_front = false;
	tabs_rebuild();
	quick_buttons_update();
	bar_clock_update();
}

/* Rebuild the styled parts when day/night flips (colors are per mode). */
static void on_mode(chumby_mode_t mode, void *user)
{
	const struct chumby_policy *p = chumby_policy();

	(void)user;
	if (panel_scr)
		build_panel(panel_scr);
	/* The mode's brightness, as last set for it (else the policy's). */
	{
		int saved = brightness_saved[mode == CHUMBY_MODE_NIGHT];

		chumby_brightness_set(saved >= 0 ? saved : mode == CHUMBY_MODE_NIGHT ?
				      p->night_brightness : p->day_brightness);
	}
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

static void shell_activity(void);
static void saver_stop(void);

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
	shell_activity();
	/* Show the volume popup as feedback while the knob turns. */
	popup_open(POPUP_VOLUME);
	popup_set(volume_pct + wheel * 2);
}

/* ------------------------------------------------------------------ */
/* Screen power                                                       */

static int screen_applied = -1;	/* what we last wrote, -1 = nothing yet */
static bool screen_on = true;	/* the power button's say */

static chumby_alarm_state_t alarm_state;

static uint64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

/* On or off as the power button left it - no schedule, no timeouts - and
 * on while an alarm rings (which leaves it on: you're up). */
static void screen_update(void)
{
	bool on = screen_on || alarm_state == CHUMBY_ALARM_RINGING;

	if (on == screen_applied)
		return;
	screen_applied = on;
	chumby_screen_power(on);
	if (!on) {
		popup_close();
		menu_close();
		saver_stop();
	}
}

static void screen_set(bool on)
{
	screen_on = on;
	screen_update();
}

/* ------------------------------------------------------------------ */
/* Screen saver                                                       */

/*
 * The screen saver (policy.conf's "saver", from SAVERS: Flying Toasters by
 * default), over everything, after SAVER_IDLE_MS without input: chumby-wm
 * says "idle" (touches, the mouse, keys: it sees those), and the knob and
 * snooze button (ours) have been still as long.
 * Not over the clock (that's what to look at, idle), a popup or menu, an
 * alarm, the calibration or a dark screen. Any input ends it ("active",
 * or ours), as an alarm does.
 */
static pid_t saver_pid = -1;
static bool wm_idle;
static uint64_t our_input_ms;	/* the knob or snooze button, last (0: never) */

static void saver_stop(void)
{
	if (saver_pid > 0)
		kill(saver_pid, SIGTERM);	/* reaped by reaper_cb */
}

/* The chosen saver's program: any program in SAVERS (on libchumby's saver
 * kit, chumby_saver.h, or not) that covers the screen with a
 * "chumby-saver" window and ends at a tap or SIGTERM. */
static const char *saver_path(void)
{
	static char path[sizeof(SAVERS) + 40];

	snprintf(path, sizeof(path), SAVERS "/%s", chumby_policy()->saver);
	return path;
}

/* Why not, if not (said to the journal when chumby-wm says "idle"). */
static const char *saver_not_now(void)
{
	if (saver_pid > 0)
		return "already up";
	if (clock_front)
		return "the clock is in front";
	if (popup_disp || menu_disp)
		return "a popup or menu is open";
	if (alarm_state == CHUMBY_ALARM_RINGING)
		return "an alarm rings";
	if (screen_applied != 1)
		return "the screen is dark";
	if (our_input_ms && mono_ms() - our_input_ms < SAVER_IDLE_MS)
		return "the knob or snooze button moved lately";
	if (!strcmp(chumby_policy()->saver, "none"))
		return "none chosen (policy.conf: saver = none)";
	if (access(saver_path(), X_OK) != 0)
		return "the chosen one isn't in " SAVERS;
	return NULL;
}

static void saver_maybe_start(bool say)
{
	char *argv[] = { (char *)chumby_policy()->saver, NULL };
	const char *why = wm_idle ? saver_not_now() : "not idle";

	if (why) {
		if (say)
			fprintf(stderr, "chumby-shell: no screen saver: %s\n", why);
		return;
	}
	saver_pid = spawn_app(saver_path(), argv, OOM_SAVER);
}

static void saver_timer_cb(lv_timer_t *t)
{
	(void)t;
	saver_maybe_start(false);
}

/* The knob or snooze button: someone's there. */
static void shell_activity(void)
{
	our_input_ms = mono_ms();
	saver_stop();
}

/* Touches: chumby-wm has them (and drops those on a dark screen, so nothing
 * gets pressed blind); ours only to be read. */
static void touch_readable(int fd, void *user)
{
	struct input_event ev;

	(void)user;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev))
		;
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

static void tone_stop(void)
{
	int i;

	if (tone_pid > 0) {
		kill(tone_pid, SIGTERM);
		/* Gone (and the sound card free) before what it paused resumes:
		 * FM's aplay doesn't retry a busy card. A second at most. */
		for (i = 0; i < 50 && waitpid(tone_pid, NULL, WNOHANG) == 0; i++)
			usleep(20 * 1000);
	}
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
	menu_close();
	if (clock)
		launch(clock);
}

static void alarm_ring(void)
{
	const struct chumby_policy *p = chumby_policy();

	saver_stop();
	char *tone_argv[] = { "chumby-alarm-tone", NULL };
	char *duck_argv[] = { "chumby-audio", "duck", "alarm", NULL };
	int v;

	if (alarm_state == CHUMBY_ALARM_RINGING)
		return;
	alarm_state = CHUMBY_ALARM_RINGING;
	ring_started = time(NULL);
	snooze_until = 0;
	/* Whatever plays (FM, a stream) pauses for the alarm, and resumes
	 * once it's dismissed (chumby-audio); the tone retries until the
	 * sound card is free. */
	spawn_app(AUDIO, duck_argv, OOM_FRONT);
	v = chumby_volume_get();
	if (v >= 0 && v < p->alarm_volume) {
		volume_before = v;
		volume_pct = p->alarm_volume;
		chumby_volume_set(p->alarm_volume);
	}
	tone_pid = spawn_app(ALARM_TONE, tone_argv, OOM_KEEP);
	alarm_publish();
	show_clock();
	screen_set(true);	/* and left on: you're up */
}

static void alarm_snooze(void)
{
	if (alarm_state != CHUMBY_ALARM_RINGING)
		return;
	char *release_argv[] = { "chumby-audio", "release", "alarm", NULL };

	tone_stop();
	/* What played before it, back on until it rings again (which pauses
	 * it again). */
	spawn_app(AUDIO, release_argv, OOM_FRONT);
	alarm_state = CHUMBY_ALARM_SNOOZED;
	snooze_until = time(NULL) + chumby_policy()->snooze_minutes * 60;
	alarm_publish();
}

static void alarm_stop(void)
{
	char *release_argv[] = { "chumby-audio", "release", "alarm", NULL };
	bool was = alarm_state != CHUMBY_ALARM_IDLE;

	tone_stop();
	if (was)	/* what played before it, back on */
		spawn_app(AUDIO, release_argv, OOM_FRONT);
	alarm_state = CHUMBY_ALARM_IDLE;
	snooze_until = 0;
	alarm_publish();
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
	 * in the same minute. Not before the clock is synced from the
	 * internet: 00:05 in 1970 could match an alarm, and even the RTC's
	 * time can be off. (Exact minutes only, so one missed until then
	 * doesn't ring late.) */
	if (minute != last_minute && chumby_time_synced()) {
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

/* Ringing: snooze. Otherwise nothing, for now (the screen is the power
 * button's). */
static void snooze_pressed(void)
{
	if (alarm_state != CHUMBY_ALARM_RINGING)
		return;
	alarm_snooze();
	show_clock();
	screen_update();
}

/*
 * Holding the snooze button CALIBRATE_HOLD_MS starts the touchscreen
 * calibration: there's no menu entry for it, and this works when touch is
 * too far off to hit one. So a plain press acts on release, unless an
 * alarm is ringing: that snoozes at once.
 */
#define CALIBRATE_HOLD_MS	4000

static int calibrate_pid = -1;
static bool snooze_down, snooze_held;
static lv_timer_t *snooze_hold_timer;

static void calibrate_start(void)
{
	char *argv[] = { "chumby-calibrate", NULL };

	if (calibrate_pid > 0 && kill(calibrate_pid, 0) == 0)
		return;		/* already up */
	popup_close();
	menu_close();
	screen_set(true);
	calibrate_pid = spawn_app(CALIBRATE, argv, OOM_FRONT);
}

static void snooze_hold_cb(lv_timer_t *t)
{
	lv_timer_pause(t);
	if (snooze_down) {
		snooze_held = true;
		calibrate_start();
	}
}

static void snooze_readable(int fd, void *user)
{
	struct input_event ev;

	(void)user;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY || ev.value == 2)		/* repeats */
			continue;
		if (ev.value == 1) {
			bool saver_up = saver_pid > 0;

			shell_activity();
			if (alarm_state == CHUMBY_ALARM_RINGING) {
				snooze_pressed();	/* at once */
				continue;
			}
			if (saver_up)
				continue;	/* that's what it was for: it's gone */
			snooze_down = true;
			snooze_held = false;
			lv_timer_reset(snooze_hold_timer);
			lv_timer_resume(snooze_hold_timer);
		} else if (snooze_down) {
			snooze_down = false;
			lv_timer_pause(snooze_hold_timer);
			if (!snooze_held)
				snooze_pressed();
		}
	}
}

/* ------------------------------------------------------------------ */
/* Power button                                                       */

/*
 * The i.MX23's power switch (the "mxs-pswitch" key, KEY_POWER): a press
 * turns the screen off, or on (with the clock); held POWER_HOLD_MS it
 * shuts the Chumby down - before the chip's own hard power-off on a long
 * hold.
 */
#define POWER_HOLD_MS	2000

static bool power_down, power_held;
static lv_timer_t *power_hold_timer;

static void power_pressed(void)
{
	if (screen_applied == 1) {
		screen_set(false);
	} else {
		screen_set(true);
		show_clock();
	}
}

static void power_hold_cb(lv_timer_t *t)
{
	char *argv[] = { "systemctl", "poweroff", NULL };

	lv_timer_pause(t);
	if (!power_down)
		return;
	power_held = true;
	fprintf(stderr, "chumby-shell: power button held: shutting down\n");
	screen_set(false);
	chumby_spawn("/usr/bin/systemctl", argv);
}

static void power_readable(int fd, void *user)
{
	struct input_event ev;

	(void)user;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY || ev.code != KEY_POWER || ev.value == 2)
			continue;
		if (ev.value == 1) {
			shell_activity();
			power_down = true;
			power_held = false;
			lv_timer_reset(power_hold_timer);
			lv_timer_resume(power_hold_timer);
		} else if (power_down) {
			power_down = false;
			lv_timer_pause(power_hold_timer);
			if (!power_held)
				power_pressed();
		}
	}
}

/* ------------------------------------------------------------------ */
/* Command FIFO: $run_dir/shell.cmd                                   */

static void command(const char *cmd)
{
	if (!strcmp(cmd, "snooze"))
		snooze_pressed();
	else if (!strcmp(cmd, "power"))		/* the power button, pressed */
		power_pressed();
	else if (!strncmp(cmd, "setting ", 8)) {	/* the Settings app: what's root's */
		char key[16] = "", val[64] = "";
		char *argv[] = { "chumby-setting", key, val, NULL };

		if (sscanf(cmd + 8, "%15s %63s", key, val) == 2)
			chumby_spawn("/usr/libexec/chumby-setting", argv);	/* (it checks them) */
	} else if (!strcmp(cmd, "restart airplay")) {	/* audio.d/airplay, as the apps' user */
		char *argv[] = { "systemctl", "restart", "shairport-sync", NULL };

		chumby_spawn("/usr/bin/systemctl", argv);
	} else if (!strcmp(cmd, "screen on"))
		screen_set(true);
	else if (!strcmp(cmd, "screen off"))
		screen_set(false);
	else if (!strcmp(cmd, "stop"))
		alarm_stop();
	else if (!strcmp(cmd, "ring"))
		alarm_ring();
	else if (!strcmp(cmd, "volume"))
		popup_open(POPUP_VOLUME);
	else if (!strcmp(cmd, "brightness"))
		popup_open(POPUP_BRIGHTNESS);
	else if (!strcmp(cmd, "menu"))
		menu_open();
	else if (!strcmp(cmd, "calibrate"))
		calibrate_start();
	else if (!strncmp(cmd, "launch ", 7)) {	/* an app to the front (AirPlay starting) */
		struct app_info *a = app_by_id(cmd + 7);

		if (a) {
			popup_close();
			menu_close();
			launch(a);
		}
	} else if (!strcmp(cmd, "dismiss")) {	/* chumby-wm: a tap outside them */
		popup_close();
		menu_close();
	} else if (!strcmp(cmd, "idle")) {	/* chumby-wm: no input a while */
		wm_idle = true;
		saver_maybe_start(true);
	} else if (!strcmp(cmd, "active")) {	/* chumby-wm: and now some */
		wm_idle = false;
		saver_stop();
	}
	else if (cmd[0])
		fprintf(stderr, "chumby-shell: unknown command '%s'\n", cmd);
}

/* Whole lines only: a read can end mid-line when commands pile up (we
 * were busy), and the rest comes with the next one ("launch FM Radio"
 * arrived as "launch " and "FM Radio" - two unknown commands). */
static void cmd_readable(int fd, void *user)
{
	static char buf[512];
	static size_t len;
	char *nl, *start;
	ssize_t n;

	(void)user;
	while ((n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
		len += n;
		buf[len] = '\0';
		start = buf;
		while ((nl = strchr(start, '\n'))) {
			*nl = '\0';
			if (*start)
				command(start);
			start = nl + 1;
		}
		len = strlen(start);
		memmove(buf, start, len + 1);
		if (len == sizeof(buf) - 1)
			len = 0;	/* a line that long isn't one of ours */
	}
}

/* Opened read-write so it never reports EOF when a writer goes away. */
static int cmd_fifo_open(void)
{
	const char *dir = chumby_run_dir();
	char path[256];
	struct stat st;

	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/shell.cmd", dir);
	/* Something else there - a plain file, made by a shell's "echo cmd >"
	 * before we were up - goes: polled, a file is always readable, and
	 * read at its end gives nothing, so we'd spin (half the ARM9) and
	 * never hear a command. */
	if (lstat(path, &st) == 0 && !S_ISFIFO(st.st_mode)) {
		fprintf(stderr, "chumby-shell: %s wasn't a FIFO: made again\n", path);
		unlink(path);
	}
	if (mkfifo(path, 0622) < 0 && errno != EEXIST) {
		fprintf(stderr, "chumby-shell: cannot create %s\n", path);
		return -1;
	}
	/* Writable by the apps (another user: "launch AirPlay" from its hook,
	 * chumby_shell_command()), whatever our umask made of it. */
	chmod(path, 0622);
	return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

/* ------------------------------------------------------------------ */
/* Memory: background apps go first                                   */

static void oom_set(int pid, int adj)
{
	char path[40], v[16];
	int fd, n;

	if (pid <= 0)
		return;
	snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
	if ((fd = open(path, O_WRONLY | O_CLOEXEC)) < 0)
		return;
	n = snprintf(v, sizeof(v), "%d", adj);
	if (write(fd, v, n) < 0)
		;
	close(fd);
}

/* The app in front 0; the clock 100; the others 300, 400... the longer
 * they've been in the background. */
static void oom_update(void)
{
	struct toplevel *t, *cur = tl_current();

	for (t = toplevels; t; t = t->next) {
		struct app_info *a = app_by_id(t->app_id);
		struct toplevel *o;
		int newer = 0;

		if (!a || a->pid <= 0)
			continue;
		if (t == cur) {
			oom_set(a->pid, OOM_FRONT);
			continue;
		}
		if (is_clock(t->app_id)) {
			oom_set(a->pid, OOM_CLOCK);
			continue;
		}
		for (o = toplevels; o; o = o->next)
			if (o != cur && o != t && !is_clock(o->app_id) && o->seen > t->seen)
				newer++;
		oom_set(a->pid, OOM_BACK + 100 * newer > 1000 ? 1000 : OOM_BACK + 100 * newer);
	}
}

/*
 * Before the kernel has to: every EVICT_EVERY_MS, if less than
 * EVICT_BELOW_KB is available, the app longest in the background is asked
 * to close (as holding its tab does), and ended EVICT_GRACE_MS later if it
 * hasn't. Never the app in front, nor the clock (the home app). What an app
 * plays goes on (chumby-stream, the FM loop, AirPlay are processes of their
 * own); its tab comes back when it's started again.
 */
#define EVICT_EVERY_MS	2000
#define EVICT_BELOW_KB	8192
#define EVICT_GRACE_MS	3000

static int evicting_pid;
static uint64_t evicting_since;

static long mem_available_kb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[80];
	long kb = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

static void evict_cb(lv_timer_t *timer)
{
	struct toplevel *t, *cur = tl_current(), *victim = NULL;
	struct app_info *a;
	long kb;

	(void)timer;
	if (evicting_pid > 0) {		/* asked to close: still there? */
		if (kill(evicting_pid, 0) == 0 && mono_ms() - evicting_since < EVICT_GRACE_MS)
			return;
		if (kill(evicting_pid, 0) == 0)
			kill(evicting_pid, SIGKILL);
		evicting_pid = 0;
	}
	kb = mem_available_kb();
	if (kb < 0 || kb >= EVICT_BELOW_KB)
		return;
	for (t = toplevels; t; t = t->next) {
		a = app_by_id(t->app_id);
		if (t == cur || is_clock(t->app_id) || !a || a->pid <= 0)
			continue;
		if (!victim || t->seen < victim->seen)
			victim = t;
	}
	if (!victim)
		return;
	a = app_by_id(victim->app_id);
	fprintf(stderr, "chumby-shell: memory low (%ld kB available): closing %s\n",
		kb, victim->app_id);
	zwlr_foreign_toplevel_handle_v1_close(victim->handle);
	wl_display_flush(wl);
	evicting_pid = a->pid;
	evicting_since = mono_ms();
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
		if (pid == saver_pid)
			saver_pid = -1;
		for (i = 0; i < n_apps; i++) {
			if (apps[i].starting_pid == pid)
				apps[i].starting_pid = 0;	/* died starting */
			if (apps[i].pid == pid)
				apps[i].pid = 0;
		}
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
	lv_display_t *panel;
	int resistive_ts;

	chumby_init();
	app_user_init();
	load_apps();
	bundles_watch();
	ftl_init();

	panel = chumby_window_create("chumby-panel", CHUMBY_SCREEN_W, CHUMBY_BAR_H);
	if (!panel) {
		fprintf(stderr, "chumby-shell: cannot create windows\n");
		return 1;
	}
	panel_scr = lv_display_get_screen_active(panel);

	popup_timer = lv_timer_create(popup_timer_cb, POPUP_IDLE_MS, NULL);
	lv_timer_pause(popup_timer);
	menu_timer = lv_timer_create(menu_timer_cb, MENU_IDLE_MS, NULL);
	lv_timer_pause(menu_timer);
	/* Builds the bar, sets the brightness. */
	brightness_load();
	chumby_on_mode_change(on_mode, NULL);
	lv_timer_create(bar_clock_cb, 1000, NULL);

	chumby_watch_fd(find_input("rotary"), knob_readable, NULL);
	chumby_watch_fd(find_input("snooze"), snooze_readable, NULL);
	chumby_watch_fd(find_input("mxs-pswitch"), power_readable, NULL);
	audio_watch();
	resistive_ts = find_input("mxs-lradc-ts");
	chumby_watch_fd(resistive_ts, touch_readable, NULL);
	chumby_watch_fd(cmd_fifo_open(), cmd_readable, NULL);
	lv_timer_ready(lv_timer_create(volume_sync_cb, 2000, NULL));
	lv_timer_create(reaper_cb, 1000, NULL);
	lv_timer_create(evict_cb, EVICT_EVERY_MS, NULL);
	lv_timer_create(saver_timer_cb, 10 * 1000, NULL);
	/* A stale state file from a previous run: we're idle. */
	alarm_publish();
	lv_timer_ready(lv_timer_create(alarm_tick_cb, 1000, NULL));

	snooze_hold_timer = lv_timer_create(snooze_hold_cb, CALIBRATE_HOLD_MS, NULL);
	lv_timer_pause(snooze_hold_timer);
	power_hold_timer = lv_timer_create(power_hold_cb, POWER_HOLD_MS, NULL);
	lv_timer_pause(power_hold_timer);
	/* Never calibrated: that first (chumby-wm shows it over everything).
	 * Only the Chumby's resistive touchscreen needs it; a capacitive one
	 * (the RasPad's) is right as it comes. */
	if (access(CAL_FILE, F_OK) != 0 && resistive_ts >= 0)
		calibrate_start();
	/* Start on the clock, not an empty desktop. */
	show_clock();

	return chumby_run();
}
