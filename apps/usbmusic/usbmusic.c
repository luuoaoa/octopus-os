// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * USB Music: plays the music on a USB stick (MP3, AAC, M4A).
 *
 * Browse the stick's folders (the list button); tap a song to play it and
 * what follows it in its folder, or a folder's play button to play
 * everything in it - its folders too, all the way down - in order. The
 * playing is the system's stream player's (octopus_stream_play() takes a
 * file's path); this app keeps the queue and moves on when a song ends,
 * so it stays awake meanwhile (octopus_stay_awake()), even out of sight.
 * The stick is mounted by the system (octopus_usb_dir()); pulled out, the
 * queue stops. The queue (its folder and where it was) is kept in the
 * app's data directory, for the next start.
 *
 * Same layout as the other players: the art top left at the margins, the
 * text right of it, the bar and times under it, the controls along the
 * bottom (sizes are 320x240's, through octopus_px()).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <octopus.h>

#define APP_ID		"UsbMusic"	/* = Id in app.conf */
#define MAX_QUEUE	2000
#define MAX_DEPTH	8		/* folders in folders, playing a folder */
#define MAX_ROWS	250		/* shown in one folder's list */
#define RESTART_S	3		/* "previous" after this long: the song again */
#define START_GRACE_S	4		/* a song just started: the player's status is still the last one's */

#define ART_X		OCTOPUS_MARGIN
#define ART_SIZE	octopus_px(92)
#define COL_X		(ART_X + ART_SIZE + OCTOPUS_MARGIN)
#define COL_W		(OCTOPUS_APP_W - OCTOPUS_MARGIN - COL_X)
#define LINE_GAP	octopus_px(2)
#define BAR_H		octopus_px(4)
#define BAR_GAP		octopus_px(8)
#define SMALL_BTN	octopus_px(38)
#define ROW_H		octopus_px(44)
#define ROW_GAP		octopus_px(6)
#define HEAD_H		octopus_px(34)
#define INFO_W		octopus_px(60)		/* "12 / 345" */

static lv_obj_t *scr;
static lv_obj_t *art_tile, *art_note, *lbl_where, *lbl_count, *lbl_title, *lbl_artist;
static lv_obj_t *bar, *lbl_elapsed, *lbl_hint;
static lv_obj_t *btn_list, *btn_prev, *btn_play, *btn_next;
static lv_obj_t *list_panel;

/* The queue: songs (full paths) from a folder, and where in it we are. */
static char *queue[MAX_QUEUE];
static int n_queue, cur = -1;
static char queue_dir[512];		/* what was played: the folder */
static bool playing;			/* the player is on one of ours */
static time_t started;			/* when we last started one */
static char state_file[512];

/* ------------------------------------------------------------------ */
/* Files                                                              */

static bool is_music(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".m4a") ||
		       !strcasecmp(dot, ".aac"));
}

/* Not shown: dot files (a Mac's "._" twins among them) and the system's. */
static bool skipped(const char *name)
{
	return name[0] == '.' || !strcasecmp(name, "System Volume Information") ||
	       !strcmp(name, "octopus-apps");
}

static int name_cmp(const void *a, const void *b)
{
	return strverscmp(*(char *const *)a, *(char *const *)b);	/* "2" before "10" */
}

/* A folder's entries, sorted: its folders (dirs) and its songs (files). */
struct listing {
	char **dirs, **files;
	int n_dirs, n_files;
};

static void listing_free(struct listing *l)
{
	int i;

	for (i = 0; i < l->n_dirs; i++)
		free(l->dirs[i]);
	for (i = 0; i < l->n_files; i++)
		free(l->files[i]);
	free(l->dirs);
	free(l->files);
	memset(l, 0, sizeof(*l));
}

static bool listing_read(const char *dir, struct listing *l)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[1024];
	struct stat st;
	int cap_d = 0, cap_f = 0;

	memset(l, 0, sizeof(*l));
	if (!d)
		return false;
	while ((e = readdir(d))) {
		if (skipped(e->d_name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (l->n_dirs == cap_d) {
				cap_d = cap_d ? cap_d * 2 : 16;
				l->dirs = realloc(l->dirs, cap_d * sizeof(char *));
			}
			l->dirs[l->n_dirs++] = strdup(e->d_name);
		} else if (S_ISREG(st.st_mode) && is_music(e->d_name)) {
			if (l->n_files == cap_f) {
				cap_f = cap_f ? cap_f * 2 : 16;
				l->files = realloc(l->files, cap_f * sizeof(char *));
			}
			l->files[l->n_files++] = strdup(e->d_name);
		}
	}
	closedir(d);
	qsort(l->dirs, l->n_dirs, sizeof(char *), name_cmp);
	qsort(l->files, l->n_files, sizeof(char *), name_cmp);
	return true;
}

static void queue_clear(void)
{
	int i;

	for (i = 0; i < n_queue; i++)
		free(queue[i]);
	n_queue = 0;
	cur = -1;
}

/* A folder's songs into the queue, then its folders' (all the way down,
 * if recursive), in order. */
static void queue_add(const char *dir, bool recursive, int depth)
{
	struct listing l;
	char path[1024];
	int i;

	if (depth > MAX_DEPTH || !listing_read(dir, &l))
		return;
	for (i = 0; i < l.n_files && n_queue < MAX_QUEUE; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, l.files[i]);
		queue[n_queue++] = strdup(path);
	}
	for (i = 0; recursive && i < l.n_dirs && n_queue < MAX_QUEUE; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, l.dirs[i]);
		queue_add(path, true, depth + 1);
	}
	listing_free(&l);
}

/* The sticks: the directories under octopus_usb_dir(). */
static int sticks(char out[][300], int max)
{
	DIR *d = opendir(octopus_usb_dir());
	struct dirent *e;
	int n = 0;

	if (!d)
		return 0;
	while ((e = readdir(d)) && n < max)
		if (e->d_name[0] != '.' && e->d_type != DT_REG)
			snprintf(out[n++], 300, "%s/%s", octopus_usb_dir(), e->d_name);
	closedir(d);
	return n;
}

static bool on_stick(const char *path)
{
	size_t n = strlen(octopus_usb_dir());

	return !strncmp(path, octopus_usb_dir(), n) && path[n] == '/';
}

/* "/run/octopus-usb/sda1/Music/Album" -> "Album"; the stick's top -> "USB stick". */
static const char *dir_title(const char *dir)
{
	const char *slash = strrchr(dir, '/');
	size_t n = strlen(octopus_usb_dir());

	if (!on_stick(dir) || !strchr(dir + n + 1, '/'))
		return "USB stick";
	return slash ? slash + 1 : dir;
}

/* The file's name without its folder and extension. */
static void file_title(const char *path, char *out, size_t size)
{
	const char *slash = strrchr(path, '/'), *dot;

	snprintf(out, size, "%s", slash ? slash + 1 : path);
	dot = strrchr(out, '.');
	if (dot && dot != out)
		out[dot - out] = '\0';
}

/* ------------------------------------------------------------------ */
/* Kept across starts: the folder played and where in it               */

static void state_save(void)
{
	FILE *f;

	if (!state_file[0] || !(f = fopen(state_file, "w")))
		return;
	if (cur >= 0)
		fprintf(f, "%s\n%s\n", queue_dir, queue[cur]);
	fclose(f);
}

static void state_load(void)
{
	char dir[512], song[1024];
	FILE *f = state_file[0] ? fopen(state_file, "r") : NULL;
	int i;

	if (!f)
		return;
	if (fgets(dir, sizeof(dir), f) && fgets(song, sizeof(song), f)) {
		dir[strcspn(dir, "\n")] = song[strcspn(song, "\n")] = '\0';
		if (access(song, R_OK) == 0) {
			snprintf(queue_dir, sizeof(queue_dir), "%s", dir);
			queue_add(dir, true, 0);
			for (i = 0; i < n_queue; i++)
				if (!strcmp(queue[i], song))
					cur = i;
			if (cur < 0)
				queue_clear();
		}
	}
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* Playing                                                            */

static void ui_update(void);

/* Is the player on our current song? (The status keeps 255 bytes of it.) */
static bool ours(const struct octopus_stream_status *st)
{
	return cur >= 0 && st->url[0] &&
	       !strncmp(st->url, queue[cur], sizeof(st->url) - 1);
}

static void play_index(int i)
{
	struct octopus_tags t;
	char name[64];

	if (i < 0 || i >= n_queue)
		return;
	cur = i;
	if (octopus_audio_tags(queue[cur], &t) && t.title[0])
		snprintf(name, sizeof(name), "%s", t.title);
	else
		file_title(queue[cur], name, sizeof(name));
	octopus_stream_play(queue[cur], name);
	playing = true;
	started = time(NULL);
	octopus_stay_awake(true);	/* to start the next one, in or out of sight */
	state_save();
	ui_update();
}

static void stop_queue(void)
{
	playing = false;
	octopus_stay_awake(false);
}

/* A folder's songs (recursive: its folders' too), from `first` (a path,
 * or NULL: from the top). */
static void play_folder(const char *dir, bool recursive, const char *first)
{
	int i, start = 0;

	queue_clear();
	snprintf(queue_dir, sizeof(queue_dir), "%s", dir);
	queue_add(dir, recursive, 0);
	for (i = 0; first && i < n_queue; i++)
		if (!strcmp(queue[i], first))
			start = i;
	if (n_queue)
		play_index(start);
	else
		ui_update();
}

static void play_cb(lv_event_t *e)
{
	(void)e;
	if (playing) {
		octopus_stream_stop();
		stop_queue();
	} else if (cur >= 0) {
		play_index(cur);
	}
	ui_update();
}

static void step_cb(lv_event_t *e)
{
	int dir = (int)(intptr_t)lv_event_get_user_data(e);
	struct octopus_stream_status st;

	if (cur < 0)
		return;
	octopus_stream_status(&st);
	if (dir < 0 && ours(&st) && st.played_s >= RESTART_S)
		play_index(cur);		/* back to this one's start */
	else if (cur + dir >= 0 && cur + dir < n_queue)
		play_index(cur + dir);
}

/* Twice a second: what the player is doing; the next song at an end. */
static void poll_cb(lv_timer_t *t)
{
	struct octopus_stream_status st;
	bool any;

	(void)t;
	any = octopus_stream_status(&st);	/* false, too, for "ended" */
	if (playing && !ours(&st) && time(NULL) - started < START_GRACE_S) {
		ui_update();		/* not ours yet: the player is starting it */
		return;
	}
	if (playing && ours(&st) && !strcmp(st.state, "ended")) {
		if (cur + 1 < n_queue) {
			play_index(cur + 1);
		} else {		/* the end of the queue: from the top, stopped */
			stop_queue();
			cur = 0;
			state_save();
		}
	} else if (playing && ours(&st) && !strcmp(st.state, "error") &&
		   access(queue[cur], R_OK) != 0) {
		octopus_stream_stop();	/* the stick is gone */
		stop_queue();
	} else if (playing && !(any && ours(&st)) && st.url[0]) {
		stop_queue();		/* something else plays now (the radio) */
	}
	ui_update();
}

/* ------------------------------------------------------------------ */
/* The screen                                                         */

static void ui_update(void)
{
	struct octopus_stream_status st;
	struct octopus_tags tags;
	static char shown[1024];	/* the song whose tags show */
	char buf[160];
	int n_sticks;
	char dummy[1][300];

	octopus_stream_status(&st);
	lv_label_set_text(lv_obj_get_child(btn_play, 0),
			  playing && ours(&st) ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
	lv_obj_set_state(btn_prev, LV_STATE_DISABLED, cur < 0);
	lv_obj_set_state(btn_next, LV_STATE_DISABLED, cur < 0 || cur + 1 >= n_queue);
	lv_obj_set_state(btn_play, LV_STATE_DISABLED, cur < 0);

	n_sticks = sticks(dummy, 1);
	if (cur < 0) {
		shown[0] = '\0';
		lv_label_set_text(lbl_where, "");
		lv_label_set_text(lbl_count, "");
		octopus_scroll_label_set(lbl_title, n_sticks ? "Nothing playing" : "No USB stick");
		lv_label_set_text(lbl_artist, "");
		lv_label_set_text(lbl_hint, n_sticks ?
				  "Tap " LV_SYMBOL_LIST " to pick a folder or a song." :
				  "Plug in a USB stick with music on it.");
		lv_obj_set_hidden(lbl_hint, false);
		lv_obj_set_hidden(bar, true);
		lv_obj_set_hidden(lbl_elapsed, true);
		return;
	}
	lv_obj_set_hidden(lbl_hint, true);
	lv_obj_set_hidden(bar, false);
	lv_obj_set_hidden(lbl_elapsed, false);
	if (strcmp(shown, queue[cur])) {	/* a new song: its tags (read once) */
		snprintf(shown, sizeof(shown), "%s", queue[cur]);
		if (!octopus_audio_tags(queue[cur], &tags))
			memset(&tags, 0, sizeof(tags));
		if (!tags.title[0])
			file_title(queue[cur], tags.title, sizeof(tags.title));
		if (tags.album[0]) {
			lv_label_set_text(lbl_where, tags.album);
		} else {	/* its folder */
			char dir[1024], *slash;

			snprintf(dir, sizeof(dir), "%s", queue[cur]);
			slash = strrchr(dir, '/');
			if (slash)
				*slash = '\0';
			lv_label_set_text(lbl_where, dir_title(dir));
		}
		octopus_scroll_label_set(lbl_title, tags.title);
		octopus_scroll_label_set(lbl_artist, tags.artist);
	}
	snprintf(buf, sizeof(buf), "%d / %d", cur + 1, n_queue);
	lv_label_set_text(lbl_count, buf);
	if (playing && ours(&st) && st.length > 0) {
		lv_bar_set_value(bar, (int)(st.byte * 1000 / st.length), LV_ANIM_OFF);
		snprintf(buf, sizeof(buf), "%d:%02d", st.played_s / 60, st.played_s % 60);
		lv_label_set_text(lbl_elapsed, !strcmp(st.state, "connecting") ? "" :
				  !strcmp(st.state, "error") ? "Can't play it" : buf);
	} else {
		lv_bar_set_value(bar, 0, LV_ANIM_OFF);
		lv_label_set_text(lbl_elapsed, "");
	}
}

/* ------------------------------------------------------------------ */
/* The browser                                                        */

static char browse_dir[1024];		/* "" = the sticks */

static void list_close(void)
{
	if (list_panel) {
		lv_obj_delete(list_panel);
		list_panel = NULL;
	}
}

static void list_show(const char *dir);

static void list_close_async(void *u)
{
	(void)u;
	list_close();
}

static void list_show_async(void *u)
{
	list_show(u);
	free(u);
}

static void open_cb(lv_event_t *e)
{
	lv_async_call(list_show_async, strdup(lv_event_get_user_data(e)));
}

static void close_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(list_close_async, NULL);
}

/* Up a folder; from a stick's top, to the sticks (with more than one). */
static void back_cb(lv_event_t *e)
{
	char up[1024], *slash, list[2][300];

	(void)e;
	snprintf(up, sizeof(up), "%s", browse_dir);
	slash = strrchr(up, '/');
	if (slash)
		*slash = '\0';
	if (!on_stick(up))
		snprintf(up, sizeof(up), "%s", sticks(list, 2) == 1 ? list[0] : "");
	lv_async_call(list_show_async, strdup(up));
}

/* User data: "<r|f>path" (r: recursive, f: this folder from a song). */
static void play_cb_list(lv_event_t *e)
{
	const char *what = lv_event_get_user_data(e);

	if (what[0] == 'r') {
		play_folder(what + 1, true, NULL);
	} else {	/* a song: its folder, from it */
		char dir[1024], *slash;

		snprintf(dir, sizeof(dir), "%s", what + 1);
		slash = strrchr(dir, '/');
		if (slash)
			*slash = '\0';
		play_folder(dir, false, what + 1);
	}
	lv_async_call(list_close_async, NULL);
}

/* The rows own their paths (freed with them). */
static void free_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static char *tagged(char kind, const char *path)
{
	char *s = malloc(strlen(path) + 2);

	s[0] = kind;
	strcpy(s + 1, path);
	return s;
}

static lv_obj_t *row(lv_obj_t *parent, const char *icon, const char *text, bool on)
{
	lv_obj_t *b = lv_button_create(parent), *l;

	lv_obj_set_size(b, OCTOPUS_APP_W - 2 * OCTOPUS_MARGIN, ROW_H);
	lv_obj_set_style_radius(b, octopus_px(8), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_pad_hor(b, octopus_px(10), 0);
	lv_obj_set_style_bg_color(b, octopus_color(OCTOPUS_COLOR_SURFACE), 0);
	lv_obj_set_style_text_color(b, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	if (on) {	/* playing: the accent */
		octopus_glossy(b, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
		lv_obj_set_style_text_color(b, octopus_on_accent(), 0);
	}
	l = lv_label_create(b);
	lv_label_set_text(l, icon);
	lv_obj_set_style_text_font(l, octopus_symbol_font(14), 0);
	lv_obj_set_width(l, octopus_px(20));
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
	l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, octopus_font(OCTOPUS_FONT_LARGE), 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_width(l, OCTOPUS_APP_W - 2 * OCTOPUS_MARGIN - octopus_px(20 + 20) - OCTOPUS_TOUCH_MIN);
	lv_obj_set_height(l, lv_font_get_line_height(octopus_font(OCTOPUS_FONT_LARGE)));
	lv_obj_align(l, LV_ALIGN_LEFT_MID, octopus_px(26), 0);
	return b;
}

/* A play button at a folder row's right end. */
static void row_play(lv_obj_t *r, const char *dir)
{
	lv_obj_t *b = lv_button_create(r), *l;
	char *what = tagged('r', dir);

	lv_obj_set_size(b, OCTOPUS_TOUCH_MIN, ROW_H - octopus_px(8));
	lv_obj_align(b, LV_ALIGN_RIGHT_MID, octopus_px(6), 0);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(b, octopus_color(OCTOPUS_COLOR_BG), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, play_cb_list, LV_EVENT_CLICKED, what);
	lv_obj_add_event_cb(b, free_cb, LV_EVENT_DELETE, what);
	l = lv_label_create(b);
	lv_label_set_text(l, LV_SYMBOL_PLAY);
	lv_obj_set_style_text_font(l, octopus_symbol_font(14), 0);
	lv_obj_center(l);
}

static void list_show(const char *dir)
{
	lv_obj_t *head, *items, *l, *b, *r;
	char path[1024], list[8][300];
	struct listing ls;
	int i, n, shown = 0;

	list_close();
	snprintf(browse_dir, sizeof(browse_dir), "%s", dir);
	list_panel = lv_obj_create(scr);
	lv_obj_remove_style_all(list_panel);
	lv_obj_set_size(list_panel, OCTOPUS_APP_W, OCTOPUS_APP_H);
	lv_obj_set_style_bg_opa(list_panel, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(list_panel, octopus_color(OCTOPUS_COLOR_BG), 0);
	lv_obj_set_scrollable(list_panel, false);

	/* Header: where we are (tap it to go up), and close. */
	head = lv_button_create(list_panel);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, OCTOPUS_APP_W - OCTOPUS_MARGIN - OCTOPUS_TOUCH_MIN, HEAD_H);
	lv_obj_set_pos(head, OCTOPUS_MARGIN, 0);
	l = lv_label_create(head);
	lv_obj_set_width(l, OCTOPUS_APP_W - OCTOPUS_MARGIN - OCTOPUS_TOUCH_MIN);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_text_font(l, octopus_font(OCTOPUS_FONT_LARGE), 0);
	lv_obj_set_style_text_color(l, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
	b = lv_button_create(list_panel);
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, OCTOPUS_TOUCH_MIN, HEAD_H);
	lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
	r = lv_label_create(b);
	lv_label_set_text(r, LV_SYMBOL_CLOSE);
	lv_obj_set_style_text_color(r, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
	lv_obj_center(r);
	lv_obj_add_event_cb(b, close_cb, LV_EVENT_CLICKED, NULL);

	items = lv_obj_create(list_panel);
	lv_obj_remove_style_all(items);
	lv_obj_set_size(items, OCTOPUS_APP_W, OCTOPUS_APP_H - HEAD_H);
	lv_obj_set_pos(items, 0, HEAD_H);
	lv_obj_set_flex_flow(items, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(items, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_bottom(items, OCTOPUS_MARGIN, 0);
	lv_obj_set_style_pad_row(items, ROW_GAP, 0);
	lv_obj_set_scroll_dir(items, LV_DIR_VER);

	if (!dir[0]) {			/* the sticks */
		lv_label_set_text(l, "USB sticks");
		n = sticks(list, 8);
		for (i = 0; i < n; i++) {
			char *p = strdup(list[i]), name[32];

			snprintf(name, sizeof(name), "USB stick %d", i + 1);
			r = row(items, LV_SYMBOL_USB, name, false);
			lv_obj_add_event_cb(r, open_cb, LV_EVENT_CLICKED, p);
			lv_obj_add_event_cb(r, free_cb, LV_EVENT_DELETE, p);
			row_play(r, list[i]);
		}
		if (!n) {
			r = lv_label_create(items);
			lv_obj_set_style_text_color(r, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
			lv_label_set_text(r, "Plug in a USB stick with music on it.");
		}
		return;
	}

	/* Up a level, but from the top of the only stick there's nowhere. */
	{
		char up[1024], *slash;

		snprintf(up, sizeof(up), "%s", dir);
		slash = strrchr(up, '/');
		if (slash)
			*slash = '\0';
		if (on_stick(up) || sticks(list, 2) > 1) {
			lv_label_set_text_fmt(l, LV_SYMBOL_LEFT "  %s", dir_title(dir));
			lv_obj_add_event_cb(head, back_cb, LV_EVENT_CLICKED, NULL);
		} else {
			lv_label_set_text(l, dir_title(dir));
		}
	}
	if (!listing_read(dir, &ls)) {
		r = lv_label_create(items);
		lv_obj_set_style_text_color(r, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
		lv_label_set_text(r, "Can't read this folder.");
		return;
	}
	if (ls.n_dirs || ls.n_files) {	/* all of it, folders and all */
		char *what = tagged('r', dir);

		r = row(items, LV_SYMBOL_PLAY, "Play all", false);
		lv_obj_add_event_cb(r, play_cb_list, LV_EVENT_CLICKED, what);
		lv_obj_add_event_cb(r, free_cb, LV_EVENT_DELETE, what);
	}
	for (i = 0; i < ls.n_dirs && shown < MAX_ROWS; i++, shown++) {
		char *p;

		snprintf(path, sizeof(path), "%s/%s", dir, ls.dirs[i]);
		p = strdup(path);
		r = row(items, LV_SYMBOL_DIRECTORY, ls.dirs[i], false);
		lv_obj_add_event_cb(r, open_cb, LV_EVENT_CLICKED, p);
		lv_obj_add_event_cb(r, free_cb, LV_EVENT_DELETE, p);
		row_play(r, path);
	}
	for (i = 0; i < ls.n_files && shown < MAX_ROWS; i++, shown++) {
		char title[256], *what;
		bool on;

		snprintf(path, sizeof(path), "%s/%s", dir, ls.files[i]);
		file_title(path, title, sizeof(title));
		on = cur >= 0 && !strcmp(queue[cur], path);
		what = tagged('f', path);
		r = row(items, LV_SYMBOL_AUDIO, title, on);
		lv_obj_add_event_cb(r, play_cb_list, LV_EVENT_CLICKED, what);
		lv_obj_add_event_cb(r, free_cb, LV_EVENT_DELETE, what);
		if (on)
			lv_obj_scroll_to_view(r, LV_ANIM_OFF);
	}
	if (ls.n_dirs + ls.n_files > shown) {
		r = lv_label_create(items);
		lv_obj_set_style_text_color(r, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
		lv_label_set_text_fmt(r, "... and %d more", ls.n_dirs + ls.n_files - shown);
	}
	if (!ls.n_dirs && !ls.n_files) {
		r = lv_label_create(items);
		lv_obj_set_style_text_color(r, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
		lv_label_set_text(r, "No music here.");
	}
	listing_free(&ls);
}

/* The list button: where the song playing is, else the stick's top (or
 * the sticks, with more than one). */
static void list_cb(lv_event_t *e)
{
	char dir[1024], *slash, list[2][300];

	(void)e;
	if (list_panel) {
		list_close();
		return;
	}
	if (cur >= 0 && access(queue[cur], R_OK) == 0) {
		snprintf(dir, sizeof(dir), "%s", queue[cur]);
		slash = strrchr(dir, '/');
		if (slash)
			*slash = '\0';
	} else {
		snprintf(dir, sizeof(dir), "%s", sticks(list, 2) == 1 ? list[0] : "");
	}
	list_show(dir);
}

/* Sticks coming and going (inotify on octopus_usb_dir()). */
static void usb_changed(int fd, void *user)
{
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));

	(void)user;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	if (list_panel && !browse_dir[0])
		list_show("");
	else if (list_panel && access(browse_dir, R_OK) != 0)
		list_show("");		/* the one we were in is gone */
	ui_update();
}

/* ------------------------------------------------------------------ */

static lv_obj_t *round_button(lv_obj_t *parent, const char *sym, int size, const lv_font_t *font)
{
	lv_obj_t *b = lv_button_create(parent), *l = lv_label_create(b);

	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_center(l);
	return b;
}

static void on_mode(octopus_mode_t mode, void *user)
{
	lv_color_t surface = octopus_color(OCTOPUS_COLOR_SURFACE);
	lv_color_t accent = octopus_color(OCTOPUS_COLOR_ACCENT);

	(void)mode;
	(void)user;
	lv_obj_set_style_bg_color(art_tile, lv_color_lighten(accent, 30), 0);
	lv_obj_set_style_bg_grad_color(art_tile, lv_color_darken(accent, 140), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_text_color(art_note, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_where, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_count, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_title, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_artist, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_elapsed, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_hint, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_bg_color(bar, surface, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, accent, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(btn_prev, surface, 0);
	lv_obj_set_style_bg_color(btn_next, surface, 0);
	lv_obj_set_style_bg_color(btn_play, surface, 0);
	lv_obj_set_style_bg_color(btn_list, surface, LV_STATE_PRESSED);
	lv_obj_set_style_text_color(btn_list, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_border_color(btn_list, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
	if (list_panel)
		list_close();
}

static lv_obj_t *text_label(int x, int y, int w, octopus_font_t font)
{
	lv_obj_t *l = lv_label_create(scr);

	lv_obj_set_style_text_font(l, octopus_font(font), 0);
	lv_obj_set_width(l, w);
	lv_obj_set_height(l, lv_font_get_line_height(octopus_font(font)));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(l, x, y);
	lv_label_set_text(l, "");
	return l;
}

int main(void)
{
	const lv_font_t *body, *large;
	lv_obj_t *row_box;
	int32_t y, fd;
	const char *data;

	scr = octopus_app_init(APP_ID);
	lv_obj_set_scrollable(scr, false);
	body = octopus_font(OCTOPUS_FONT_BODY);
	large = octopus_font(OCTOPUS_FONT_LARGE);

	/* Bottom: the browser (small), previous, play/stop, next - as far
	 * apart as on 320x240 on a wider screen too. */
	row_box = lv_obj_create(scr);
	lv_obj_remove_style_all(row_box);
	lv_obj_set_scrollable(row_box, false);
	lv_obj_set_size(row_box, octopus_px(320), octopus_px(56));
	lv_obj_align(row_box, LV_ALIGN_BOTTOM_MID, 0, -octopus_px(8));
	lv_obj_set_flex_flow(row_box, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row_box, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	btn_list = round_button(row_box, LV_SYMBOL_LIST, SMALL_BTN, octopus_symbol_font(14));
	lv_obj_set_style_bg_opa(btn_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(btn_list, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(btn_list, 1, 0);
	lv_obj_set_style_border_opa(btn_list, LV_OPA_40, 0);
	lv_obj_set_ext_click_area(btn_list, (OCTOPUS_TOUCH_MIN - SMALL_BTN) / 2);
	lv_obj_add_event_cb(btn_list, list_cb, LV_EVENT_CLICKED, NULL);
	btn_prev = round_button(row_box, LV_SYMBOL_PREV, octopus_px(52), octopus_symbol_font(28));
	lv_obj_add_event_cb(btn_prev, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
	btn_play = round_button(row_box, LV_SYMBOL_PLAY, octopus_px(56), octopus_symbol_font(28));
	lv_obj_add_event_cb(btn_play, play_cb, LV_EVENT_CLICKED, NULL);
	btn_next = round_button(row_box, LV_SYMBOL_NEXT, octopus_px(52), octopus_symbol_font(28));
	lv_obj_add_event_cb(btn_next, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

	/* The art: no pictures from the files, a note on the app's hue. */
	art_tile = lv_obj_create(scr);
	lv_obj_remove_style_all(art_tile);
	lv_obj_set_pos(art_tile, ART_X, OCTOPUS_MARGIN);
	lv_obj_set_size(art_tile, ART_SIZE, ART_SIZE);
	lv_obj_set_style_radius(art_tile, octopus_px(10), 0);
	lv_obj_set_style_bg_opa(art_tile, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(art_tile, false);
	lv_obj_set_clickable(art_tile, false);
	art_note = lv_label_create(art_tile);
	lv_label_set_text(art_note, LV_SYMBOL_AUDIO);
	lv_obj_set_style_text_font(art_note, octopus_symbol_font(48), 0);
	lv_obj_center(art_note);

	/* Right of it: the album (or folder) and "3 / 12", the title, the
	 * artist. */
	y = OCTOPUS_MARGIN;
	lbl_where = text_label(COL_X, y, COL_W - INFO_W - octopus_px(6), OCTOPUS_FONT_BODY);
	lbl_count = text_label(COL_X + COL_W - INFO_W, y, INFO_W, OCTOPUS_FONT_BODY);
	lv_obj_set_style_text_align(lbl_count, LV_TEXT_ALIGN_RIGHT, 0);
	y += lv_font_get_line_height(body) + LINE_GAP;
	lbl_title = text_label(COL_X, y, COL_W, OCTOPUS_FONT_LARGE);
	octopus_scroll_label_init(lbl_title);
	y += lv_font_get_line_height(large);
	lbl_artist = text_label(COL_X, y, COL_W, OCTOPUS_FONT_LARGE);
	octopus_scroll_label_init(lbl_artist);

	/* Under the art: how far into the song, and how long it's played. */
	y = OCTOPUS_MARGIN + ART_SIZE + BAR_GAP;
	bar = lv_bar_create(scr);
	lv_obj_set_size(bar, OCTOPUS_APP_W - 2 * OCTOPUS_MARGIN, BAR_H);
	lv_obj_set_pos(bar, OCTOPUS_MARGIN, y);
	lv_bar_set_range(bar, 0, 1000);
	lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_MAIN);
	lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_INDICATOR);
	lbl_elapsed = text_label(OCTOPUS_MARGIN, y + BAR_H + LINE_GAP,
				 OCTOPUS_APP_W - 2 * OCTOPUS_MARGIN, OCTOPUS_FONT_BODY);

	/* Nothing to play yet: what to do, where the bar would be. */
	lbl_hint = lv_label_create(scr);
	lv_obj_set_width(lbl_hint, COL_W);
	lv_label_set_long_mode(lbl_hint, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(lbl_hint, body, 0);
	lv_obj_set_pos(lbl_hint, COL_X, OCTOPUS_MARGIN + lv_font_get_line_height(body) + LINE_GAP +
		       lv_font_get_line_height(large) + LINE_GAP);

	/* Where we were last time, if that stick is in. */
	data = octopus_app_data_dir();
	if (!data)
		data = "/var/lib/octopus/apps/" APP_ID;
	mkdir(data, 0755);
	snprintf(state_file, sizeof(state_file), "%s/last", data);
	state_load();

	fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd >= 0 && inotify_add_watch(fd, octopus_usb_dir(), IN_CREATE | IN_DELETE |
					 IN_MOVED_TO | IN_MOVED_FROM) >= 0)
		octopus_watch_fd(fd, usb_changed, NULL);

	octopus_on_mode_change(on_mode, NULL);
	{	/* already playing one of ours (the app restarted)? */
		struct octopus_stream_status st;

		if (octopus_stream_status(&st) && ours(&st)) {
			playing = true;
			octopus_stay_awake(true);
		}
	}
	ui_update();
	lv_timer_create(poll_cb, 500, NULL);
	return octopus_run();
}
