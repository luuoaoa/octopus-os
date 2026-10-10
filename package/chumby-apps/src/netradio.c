// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-netradio: internet radio, laid out like the FM app:
 *
 *   left        station logo
 *   right       its category, and the stream's codec and bitrate at the
 *               right (where FM has its signal bars), small, level with
 *               the logo's top; under it, close together, the station's
 *               name and what's playing (the stream's ICY title, or how
 *               it's going)
 *   bottom      station list, previous station, play/stop, next station
 *
 * The name and what's playing scroll twice when they change (if too long),
 * then stop, cut with dots.
 *
 * Stations: "name | stream URL | logo URL" lines under "[Category]"
 * headers in /var/lib/chumby/streams.conf, else the defaults in
 * /usr/share/chumby/streams.conf. The list shows the categories as
 * tiles, then a category's stations: few enough per screen not to
 * scroll much. Logos are fetched once into /var/lib/chumby/art; a note on
 * a gradient tile until then (or when there is none).
 *
 * chumby-stream does the playing, in the background: it carries on when
 * this app closes, and tells us how it's doing through $run_dir/stream.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <chumby.h>

#define STATIONS_USER	"/var/lib/chumby/streams.conf"
#define STATIONS_DEFAULT "/usr/share/chumby/streams.conf"
#define STATION_FILE	"/var/lib/chumby/stream.station"	/* last URL */
#define ART_DIR		"/var/lib/chumby/art"
#define STREAM_TOOL	"/usr/bin/chumby-stream"
#define CURL		"/usr/bin/curl"
#define CA_BUNDLE	"/etc/ssl/certs/ca-certificates.crt"
#define MAX_STATIONS	64
#define MAX_CATS	16
#define ART_X		14
#define ART_SIZE	92
#define ART_PAD		8
#define COL_X		(ART_X + ART_SIZE + 12)	/* the right column */
#define COL_W		(CHUMBY_APP_W - 10 - COL_X)
#define SMALL_BTN	38
#define INFO_W		70			/* "AAC 320k" */
#define START_GRACE_MS	4000	/* a stream just started: no status yet */
#define LINE_GAP	2	/* between the category and the name */
#define TILE_H		44
#define TILE_GAP	6
#define LIST_HEAD_H	30

struct station {
	char name[64];
	char url[512];
	char logo[512];
	int cat;
	bool logo_tried;	/* fetched (or failed) this run */
};

static struct station stations[MAX_STATIONS];
static int n_stations, cur;
static char cats[MAX_CATS][32];
static int n_cats;
static bool playing;			/* we want it on (corrected by status) */
static uint64_t start_grace_until;
static char st_state[16], st_url[512], st_title[256], st_codec[8];
static int st_bitrate;
static char message[96];
static int logo_child = -1, logo_station = -1, stream_child = -1;
static char logo_tmp[300];

static lv_obj_t *scr, *lbl_name, *lbl_cat, *lbl_info, *lbl_song, *art_tile, *art_img, *art_note;
static lv_obj_t *btn_play, *btn_prev, *btn_next, *btn_list;
static lv_obj_t *list_panel;
static lv_timer_t *switch_timer;

static uint64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

static int cat_index(const char *name)
{
	int i;

	for (i = 0; i < n_cats; i++)
		if (!strcmp(cats[i], name))
			return i;
	if (n_cats == MAX_CATS)
		return n_cats - 1;
	snprintf(cats[n_cats], sizeof(cats[0]), "%s", name);
	return n_cats++;
}

static void load_stations(void)
{
	FILE *f = fopen(STATIONS_USER, "r");
	char line[1200], *a, *b, *c;
	int cat = -1;

	if (!f)
		f = fopen(STATIONS_DEFAULT, "r");
	n_stations = n_cats = 0;
	while (f && n_stations < MAX_STATIONS && fgets(line, sizeof(line), f)) {
		struct station *s = &stations[n_stations];

		a = trim(line);
		if (!*a || *a == '#')
			continue;
		if (*a == '[') {			/* [Category] */
			b = strchr(a, ']');
			if (b)
				*b = '\0';
			cat = cat_index(trim(a + 1));
			continue;
		}
		b = strchr(a, '|');
		if (!b)
			continue;
		*b++ = '\0';
		c = strchr(b, '|');
		if (c)
			*c++ = '\0';
		memset(s, 0, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "%s", trim(a));
		snprintf(s->url, sizeof(s->url), "%s", trim(b));
		if (c)
			snprintf(s->logo, sizeof(s->logo), "%s", trim(c));
		if (cat < 0)
			cat = cat_index("Stations");
		s->cat = cat;
		if (s->url[0])
			n_stations++;
	}
	if (f)
		fclose(f);
}

static int station_by_url(const char *url)
{
	int i;

	for (i = 0; i < n_stations; i++)
		if (!strcmp(stations[i].url, url))
			return i;
	return -1;
}

static void load_current(void)
{
	FILE *f = fopen(STATION_FILE, "r");
	char url[512] = "";
	int i;

	if (f) {
		if (fgets(url, sizeof(url), f))
			url[strcspn(url, "\n")] = '\0';
		fclose(f);
	}
	i = station_by_url(url);
	cur = i >= 0 ? i : 0;
}

static void save_current(void)
{
	FILE *f;

	if (!n_stations)
		return;
	f = fopen(STATION_FILE, "w");
	if (f) {
		fprintf(f, "%s\n", stations[cur].url);
		fclose(f);
	}
}

/* ------------------------------------------------------------------ */
/* Art                                                                */

static void art_show_note(void)
{
	lv_obj_set_hidden(art_img, true);
	lv_obj_set_hidden(art_note, false);
	lv_obj_set_style_bg_color(art_tile, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(art_note, chumby_on_accent(), 0);
	lv_obj_set_style_bg_grad_color(art_tile, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_VER, 0);
}

/* Cache name for a logo URL: a hash of it. */
static void logo_base(const struct station *s, char *out, size_t size)
{
	unsigned long h = 5381;
	const char *p;

	for (p = s->logo; *p; p++)
		h = h * 33 + (unsigned char)*p;
	snprintf(out, size, ART_DIR "/stream-%08lx", h & 0xffffffffUL);
}

/* The cached logo file, if there is one. */
static bool logo_path(const struct station *s, char *out, size_t size)
{
	static const char *exts[] = { ".png", ".jpg" };
	char base[200];
	size_t i;

	if (!s->logo[0])
		return false;
	logo_base(s, base, sizeof(base));
	for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		snprintf(out, size, "%s%s", base, exts[i]);
		if (access(out, R_OK) == 0)
			return true;
	}
	return false;
}

static void logo_fetch(int i)
{
	struct station *s = &stations[i];
	char base[200];
	char *argv[] = { "curl", "-sfL", "-m", "20", "--cacert", CA_BUNDLE,
			 "-A", "chumby-netradio/1.0", "-o", logo_tmp, s->logo, NULL };

	if (logo_child > 0 || s->logo_tried || !s->logo[0])
		return;
	s->logo_tried = true;
	mkdir(ART_DIR, 0755);
	logo_base(s, base, sizeof(base));
	snprintf(logo_tmp, sizeof(logo_tmp), "%s.tmp", base);
	logo_station = i;
	logo_child = chumby_spawn(CURL, argv);
}

/* Name the fetched file by what it is: LVGL picks decoders by extension. */
static void logo_fetched(bool ok)
{
	struct station *s = &stations[logo_station];
	unsigned char magic[4] = { 0 };
	char base[200], path[220];
	const char *ext = NULL;
	FILE *f = ok ? fopen(logo_tmp, "rb") : NULL;

	if (f) {
		if (fread(magic, 1, 4, f) == 4) {
			if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G')
				ext = ".png";
			else if (magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)
				ext = ".jpg";
		}
		fclose(f);
	}
	if (ext) {
		logo_base(s, base, sizeof(base));
		snprintf(path, sizeof(path), "%s%s", base, ext);
		rename(logo_tmp, path);
	} else {
		unlink(logo_tmp);
	}
}

static void art_update(void)
{
	char path[220];

	if (!n_stations || !logo_path(&stations[cur], path, sizeof(path)) ||
	    !chumby_logo_set(art_img, path, ART_SIZE - 2 * ART_PAD)) {
		art_show_note();
		if (n_stations)
			logo_fetch(cur);
		return;
	}
	lv_obj_center(art_img);
	lv_obj_set_hidden(art_img, false);
	lv_obj_set_hidden(art_note, true);
	/* Logos are made for a white background, like album art. */
	lv_obj_set_style_bg_color(art_tile, lv_color_white(), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_NONE, 0);
}

/* ------------------------------------------------------------------ */
/* Playing                                                            */

/* Is the running stream this station? */
static bool stream_is_current(void)
{
	return n_stations && st_url[0] && !strcmp(st_url, stations[cur].url);
}

static void ui_update(void)
{
	const char *song = "";
	char info[24] = "";
	bool live = playing && stream_is_current();

	if (message[0])
		song = message;
	else if (live && !strcmp(st_state, "playing"))
		song = st_title;
	else if (live && !strcmp(st_state, "error"))
		song = "can't reach the station, retrying...";
	else if (playing)
		song = "connecting...";
	chumby_scroll_label_set(lbl_name, n_stations ? stations[cur].name : "No stations");
	chumby_scroll_label_set(lbl_song, song);
	lv_label_set_text(lbl_cat, n_stations ? cats[stations[cur].cat] : "");
	if (live && st_codec[0] && st_bitrate > 0)
		snprintf(info, sizeof(info), "%s %dk", st_codec, st_bitrate);
	else if (live && st_codec[0])
		snprintf(info, sizeof(info), "%s", st_codec);
	if (strcmp(lv_label_get_text(lbl_info), info))
		lv_label_set_text(lbl_info, info);
	lv_label_set_text(lv_obj_get_child(btn_play, 0), playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
	if (playing)
		lv_obj_add_state(btn_play, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(btn_play, LV_STATE_CHECKED);
}

/* chumby-stream stops the old stream (up to a second), then forks into
 * the background: reaped in poll_cb, so the UI never waits. */
static void stream_run(char **argv)
{
	int pid = chumby_spawn(STREAM_TOOL, argv);

	if (pid > 0)
		stream_child = pid;
}

static void stream_start(void)
{
	char *argv[] = { "chumby-stream", stations[cur].url, stations[cur].name, NULL };

	if (!n_stations)
		return;
	stream_run(argv);
	start_grace_until = mono_ms() + START_GRACE_MS;
	snprintf(st_url, sizeof(st_url), "%s", stations[cur].url);
	snprintf(st_state, sizeof(st_state), "connecting");
	st_title[0] = st_codec[0] = '\0';
	st_bitrate = 0;
}

static void stream_stop(void)
{
	char *argv[] = { "chumby-stream", "stop", NULL };

	stream_run(argv);
	start_grace_until = mono_ms() + START_GRACE_MS;
	st_url[0] = st_state[0] = st_title[0] = st_codec[0] = '\0';
	st_bitrate = 0;
}

/* What chumby-stream says. */
static void status_read(void)
{
	char path[256], line[600], state[16] = "", url[512] = "", title[256] = "", codec[8] = "";
	int bitrate = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/stream", chumby_run_dir());
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "state=", 6))
			snprintf(state, sizeof(state), "%.15s", line + 6);
		else if (!strncmp(line, "url=", 4))
			snprintf(url, sizeof(url), "%.511s", line + 4);
		else if (!strncmp(line, "title=", 6))
			snprintf(title, sizeof(title), "%.255s", line + 6);
		else if (!strncmp(line, "codec=", 6))
			snprintf(codec, sizeof(codec), "%.7s", line + 6);
		else if (!strncmp(line, "bitrate=", 8))
			bitrate = atoi(line + 8);
	}
	if (f)
		fclose(f);
	/* chumby-stream plays podcasts too: a stream that isn't one of our
	 * stations is someone else's, as good as none for us. */
	if (!f || station_by_url(url) < 0) {
		/* Not running: stopped elsewhere (FM, an alarm, a podcast),
		 * unless just started and not up yet. */
		if (playing && mono_ms() > start_grace_until) {
			playing = false;
			st_url[0] = st_state[0] = st_title[0] = st_codec[0] = '\0';
			st_bitrate = 0;
			ui_update();
		}
		return;
	}
	if (!playing && mono_ms() > start_grace_until)
		playing = true;		/* started (or resumed) elsewhere */
	snprintf(st_state, sizeof(st_state), "%s", state);
	snprintf(st_url, sizeof(st_url), "%s", url);
	snprintf(st_title, sizeof(st_title), "%s", title);
	snprintf(st_codec, sizeof(st_codec), "%s", codec);
	st_bitrate = bitrate;
	ui_update();
}

static void poll_cb(lv_timer_t *t)
{
	int status;

	(void)t;
	if (stream_child > 0 && waitpid(stream_child, &status, WNOHANG) == stream_child)
		stream_child = -1;
	if (logo_child > 0 && waitpid(logo_child, &status, WNOHANG) == logo_child) {
		logo_child = -1;
		logo_fetched(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		if (logo_station == cur)
			art_update();
	}
	status_read();
}

/* The station settled: switch the stream to it. */
static void switch_timer_cb(lv_timer_t *t)
{
	(void)t;
	lv_timer_pause(switch_timer);
	save_current();
	if (playing)
		stream_start();
	ui_update();
}

static void select_station(int i)
{
	if (!n_stations)
		return;
	cur = (i + n_stations) % n_stations;
	message[0] = '\0';
	art_update();
	ui_update();
	/* Stepping through: switch once it settles. */
	lv_timer_reset(switch_timer);
	lv_timer_resume(switch_timer);
}

static void station_cb(lv_event_t *e)
{
	select_station(cur + (int)(intptr_t)lv_event_get_user_data(e));
}

static void play_cb(lv_event_t *e)
{
	(void)e;
	if (!n_stations)
		return;
	message[0] = '\0';
	playing = !playing;
	if (playing) {
		save_current();
		stream_start();
	} else {
		stream_stop();
	}
	ui_update();
}

/* ------------------------------------------------------------------ */
/* Station list: categories, then a category's stations, as tiles     */

static void list_show(int cat);

static void list_close(void)
{
	if (list_panel)
		lv_obj_delete(list_panel);
	list_panel = NULL;
}

static void list_close_async(void *user)
{
	(void)user;
	list_close();
}

static void list_show_async(void *cat)
{
	list_show((int)(intptr_t)cat);
}

static void station_tile_cb(lv_event_t *e)
{
	cur = (int)(intptr_t)lv_event_get_user_data(e);
	save_current();
	lv_timer_pause(switch_timer);
	message[0] = '\0';
	art_update();
	if (!playing || !stream_is_current()) {
		playing = true;
		stream_start();
	}
	ui_update();
	lv_async_call(list_close_async, NULL);	/* not from inside its own event */
}

static void cat_tile_cb(lv_event_t *e)
{
	lv_async_call(list_show_async, lv_event_get_user_data(e));
}

static void back_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(list_show_async, (void *)(intptr_t)-1);
}

static void close_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(list_close_async, NULL);
}

static lv_obj_t *tile(lv_obj_t *parent, const char *text, const char *sub, bool on)
{
	lv_obj_t *b = lv_button_create(parent), *l;

	lv_obj_set_size(b, (CHUMBY_APP_W - 3 * 8) / 2, TILE_H);
	lv_obj_set_style_radius(b, 8, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_pad_hor(b, 8, 0);
	lv_obj_set_style_bg_color(b, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_text_color(b, chumby_color(CHUMBY_COLOR_TEXT), 0);
	if (on) {	/* the selected one: the accent, glossy */
		chumby_glossy(b, chumby_color(CHUMBY_COLOR_ACCENT), 0);
		lv_obj_set_style_text_color(b, chumby_on_accent(), 0);
	}
	l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(l, (CHUMBY_APP_W - 3 * 8) / 2 - 16 - (sub ? 24 : 0));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	/* Up to two lines (a tile has room), the rest cut with dots. */
	lv_obj_update_layout(l);
	if (lv_obj_get_height(l) > 2 * lv_font_get_line_height(chumby_font(CHUMBY_FONT_LARGE))) {
		lv_obj_set_height(l, 2 * lv_font_get_line_height(chumby_font(CHUMBY_FONT_LARGE)));
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	}
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
	if (sub) {
		l = lv_label_create(b);
		lv_label_set_text(l, sub);
		lv_obj_set_style_text_color(l, on ? chumby_on_accent() : chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
		lv_obj_align(l, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	return b;
}

/* cat < 0: the categories; else that category's stations. */
static void list_show(int cat)
{
	lv_obj_t *head, *grid, *l, *b;
	char sub[8];
	int i, n;

	list_close();
	list_panel = lv_obj_create(scr);
	lv_obj_remove_style_all(list_panel);
	lv_obj_set_size(list_panel, CHUMBY_APP_W, CHUMBY_APP_H);
	lv_obj_set_style_bg_opa(list_panel, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(list_panel, chumby_color(CHUMBY_COLOR_BG), 0);
	lv_obj_set_scrollable(list_panel, false);

	/* Header: where we are (tap it to go back up), and close. */
	head = lv_button_create(list_panel);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, CHUMBY_APP_W - 8 - CHUMBY_TOUCH_MIN, LIST_HEAD_H + 4);
	lv_obj_set_pos(head, 8, 0);
	l = lv_label_create(head);
	lv_label_set_text_fmt(l, "%s%s", cat < 0 ? "" : LV_SYMBOL_LEFT "  ",
			      cat < 0 ? "Stations" : cats[cat]);
	lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_style_text_color(l, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
	if (cat >= 0)
		lv_obj_add_event_cb(head, back_cb, LV_EVENT_CLICKED, NULL);
	b = lv_button_create(list_panel);
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, CHUMBY_TOUCH_MIN, LIST_HEAD_H + 4);
	lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
	l = lv_label_create(b);
	lv_label_set_text(l, LV_SYMBOL_CLOSE);
	lv_obj_set_style_text_color(l, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, close_cb, LV_EVENT_CLICKED, NULL);

	grid = lv_obj_create(list_panel);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, CHUMBY_APP_W, CHUMBY_APP_H - LIST_HEAD_H - 4);
	lv_obj_set_pos(grid, 0, LIST_HEAD_H + 4);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_hor(grid, 8, 0);
	lv_obj_set_style_pad_bottom(grid, 8, 0);
	lv_obj_set_style_pad_gap(grid, TILE_GAP, 0);
	lv_obj_set_style_pad_column(grid, 8, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	if (cat < 0) {
		for (i = 0; i < n_cats; i++) {
			int j;

			for (j = n = 0; j < n_stations; j++)
				n += stations[j].cat == i;
			snprintf(sub, sizeof(sub), "%d", n);
			b = tile(grid, cats[i], sub, n_stations && stations[cur].cat == i);
			lv_obj_add_event_cb(b, cat_tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		}
		return;
	}
	for (i = 0; i < n_stations; i++) {
		if (stations[i].cat != cat)
			continue;
		b = tile(grid, stations[i].name, NULL, i == cur);
		lv_obj_add_event_cb(b, station_tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		if (i == cur)
			lv_obj_scroll_to_view(b, LV_ANIM_OFF);
	}
}

static void list_cb(lv_event_t *e)
{
	(void)e;
	if (list_panel)
		list_close();
	else	/* straight into the playing station's category */
		list_show(n_cats > 1 && n_stations ? stations[cur].cat : -1);
}

/* ------------------------------------------------------------------ */

static lv_obj_t *round_button(lv_obj_t *parent, const char *sym, int size,
			      const lv_font_t *font)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_center(l);
	return b;
}

static void on_mode(chumby_mode_t mode, void *user)
{
	lv_color_t surface = chumby_color(CHUMBY_COLOR_SURFACE);

	(void)user;
	lv_obj_set_style_text_color(lbl_name, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_cat, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_info, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_song, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(art_note, chumby_color(CHUMBY_COLOR_TEXT), 0);
	/* A white logo tile would glare at night. */
	lv_obj_set_style_opa(art_tile, mode == CHUMBY_MODE_NIGHT ? LV_OPA_50 : LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(btn_prev, surface, 0);
	lv_obj_set_style_bg_color(btn_next, surface, 0);
	lv_obj_set_style_bg_color(btn_play, surface, 0);
	lv_obj_set_style_bg_color(btn_list, surface, LV_STATE_PRESSED);
	lv_obj_set_style_text_color(btn_list, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_border_color(btn_list, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	if (!lv_obj_is_hidden(art_note))
		art_show_note();	/* gradient in the new palette */
	if (list_panel)
		list_close();
}

int main(void)
{
	lv_obj_t *row;
	int32_t art_y, name_y;

	scr = chumby_app_init("Internet Radio");
	lv_obj_set_scrollable(scr, false);

	/* Bottom: the station list (small), previous, play/stop, next. */
	row = lv_obj_create(scr);
	lv_obj_remove_style_all(row);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_size(row, CHUMBY_APP_W, 56);
	lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	btn_list = round_button(row, LV_SYMBOL_LIST, SMALL_BTN, &lv_font_montserrat_14);
	lv_obj_set_style_bg_opa(btn_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(btn_list, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(btn_list, 1, 0);
	lv_obj_set_style_border_opa(btn_list, LV_OPA_40, 0);
	lv_obj_set_ext_click_area(btn_list, (CHUMBY_TOUCH_MIN - SMALL_BTN) / 2);
	lv_obj_add_event_cb(btn_list, list_cb, LV_EVENT_CLICKED, NULL);
	btn_prev = round_button(row, LV_SYMBOL_PREV, 52, &lv_font_montserrat_28);
	lv_obj_add_event_cb(btn_prev, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
	btn_play = round_button(row, LV_SYMBOL_PLAY, 56, &lv_font_montserrat_28);
	lv_obj_add_event_cb(btn_play, play_cb, LV_EVENT_CLICKED, NULL);
	btn_next = round_button(row, LV_SYMBOL_NEXT, 52, &lv_font_montserrat_28);
	lv_obj_add_event_cb(btn_next, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
	lv_obj_update_layout(scr);

	/* The logo, centred in the space above the buttons. */
	art_y = (lv_obj_get_y(row) - ART_SIZE) / 2;
	art_tile = lv_obj_create(scr);
	lv_obj_remove_style_all(art_tile);
	lv_obj_set_pos(art_tile, ART_X, art_y);
	lv_obj_set_size(art_tile, ART_SIZE, ART_SIZE);
	lv_obj_set_style_radius(art_tile, 10, 0);
	lv_obj_set_style_bg_opa(art_tile, LV_OPA_COVER, 0);
	lv_obj_set_style_clip_corner(art_tile, true, 0);
	lv_obj_set_clickable(art_tile, false);
	lv_obj_set_scrollable(art_tile, false);
	art_img = lv_image_create(art_tile);
	lv_obj_set_hidden(art_img, true);
	art_note = lv_label_create(art_tile);
	lv_label_set_text(art_note, LV_SYMBOL_AUDIO);
	lv_obj_set_style_text_font(art_note, &lv_font_montserrat_48, 0);
	lv_obj_center(art_note);

	/* Right column, top, level with the logo's top: the category, and
	 * the codec and bitrate at the right (where FM has its signal bars),
	 * small. */
	lbl_cat = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_cat, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_width(lbl_cat, COL_W - INFO_W - 6);
	lv_obj_set_height(lbl_cat, lv_font_get_line_height(chumby_font(CHUMBY_FONT_BODY)));
	lv_label_set_long_mode(lbl_cat, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(lbl_cat, COL_X, art_y);
	lv_label_set_text(lbl_cat, "");
	lbl_info = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_info, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_width(lbl_info, INFO_W);
	lv_obj_set_style_text_align(lbl_info, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_text(lbl_info, "");
	lv_obj_set_pos(lbl_info, COL_X + COL_W - INFO_W, art_y);

	/* Under it, close together: the station's name, what's playing. */
	lbl_name = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_name, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(lbl_name, COL_W);
	chumby_scroll_label_init(lbl_name);
	name_y = art_y + lv_font_get_line_height(chumby_font(CHUMBY_FONT_BODY)) + LINE_GAP;
	lv_obj_set_pos(lbl_name, COL_X, name_y);
	lbl_song = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_song, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(lbl_song, COL_W);
	chumby_scroll_label_init(lbl_song);
	lv_obj_set_pos(lbl_song, COL_X, name_y + lv_font_get_line_height(chumby_font(CHUMBY_FONT_LARGE)));

	switch_timer = lv_timer_create(switch_timer_cb, 600, NULL);
	lv_timer_pause(switch_timer);

	load_stations();
	load_current();
	chumby_on_mode_change(on_mode, NULL);
	status_read();		/* already playing? which station? */
	if (playing && station_by_url(st_url) >= 0)
		cur = station_by_url(st_url);
	art_update();
	ui_update();
	lv_timer_create(poll_cb, 500, NULL);

	return chumby_run();
}
