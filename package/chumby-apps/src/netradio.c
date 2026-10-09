// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-netradio: internet radio, laid out like the FM app:
 *
 *   top right   the station list
 *   middle      station logo | station name
 *   title       what's playing (the stream's ICY title), or how it's going;
 *               scrolls when too long
 *   bottom      previous station, play/stop, next station
 *
 * Stations: "name | stream URL | logo URL" lines in
 * /var/lib/chumby/streams.conf, else the defaults in
 * /usr/share/chumby/streams.conf. Logos are fetched once into
 * /var/lib/chumby/art; a note on a gradient tile until then (or when
 * there is none).
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
#define ART_SIZE	92
#define ART_PAD		8
#define START_GRACE_MS	4000	/* a stream just started: no status yet */

struct station {
	char name[64];
	char url[512];
	char logo[512];
	bool logo_tried;	/* fetched (or failed) this run */
};

static struct station stations[MAX_STATIONS];
static int n_stations, cur;
static bool playing;			/* we want it on (corrected by status) */
static uint64_t start_grace_until;
static char st_state[16], st_url[512], st_title[256];
static char message[96];
static int logo_child = -1, logo_station = -1;
static char logo_tmp[300];

static lv_obj_t *scr, *lbl_name, *lbl_title, *art_tile, *art_img, *art_note;
static lv_obj_t *btn_play, *lbl_play, *btn_prev, *btn_next, *btn_list;
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

static void load_stations(void)
{
	FILE *f = fopen(STATIONS_USER, "r");
	char line[1200], *a, *b, *c;

	if (!f)
		f = fopen(STATIONS_DEFAULT, "r");
	n_stations = 0;
	while (f && n_stations < MAX_STATIONS && fgets(line, sizeof(line), f)) {
		struct station *s = &stations[n_stations];

		a = trim(line);
		if (!*a || *a == '#')
			continue;
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

static void title_update(void)
{
	char text[300];
	bool cur_stream = stream_is_current();

	if (message[0])
		snprintf(text, sizeof(text), "%s", message);
	else if (!playing)
		snprintf(text, sizeof(text), "%s", "");
	else if (cur_stream && !strcmp(st_state, "playing") && st_title[0])
		snprintf(text, sizeof(text), "%s", st_title);
	else if (cur_stream && !strcmp(st_state, "playing"))
		snprintf(text, sizeof(text), "%s", "");
	else if (cur_stream && !strcmp(st_state, "error"))
		snprintf(text, sizeof(text), "%s", "can't reach the station, retrying...");
	else
		snprintf(text, sizeof(text), "%s", "connecting...");
	if (strcmp(lv_label_get_text(lbl_title), text)) {
		chumby_label_scroll_speed(lbl_title, text, CHUMBY_SCROLL_SPEED);
		lv_label_set_text(lbl_title, text);
	}
}

static void ui_update(void)
{
	lv_label_set_text(lbl_name, n_stations ? stations[cur].name : "No stations");
	lv_label_set_text(lbl_play, playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
	if (playing)
		lv_obj_add_state(btn_play, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(btn_play, LV_STATE_CHECKED);
	title_update();
}

/* chumby-stream stops the old stream (up to a second), then forks into
 * the background: reaped in poll_cb, so the UI never waits. */
static int stream_child = -1;

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
	st_title[0] = '\0';
}

static void stream_stop(void)
{
	char *argv[] = { "chumby-stream", "stop", NULL };

	stream_run(argv);
	start_grace_until = mono_ms() + START_GRACE_MS;
	st_url[0] = st_state[0] = st_title[0] = '\0';
}

/* What chumby-stream says. */
static void status_read(void)
{
	char path[256], line[600], state[16] = "", url[512] = "", title[256] = "";
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
	}
	if (f)
		fclose(f);
	if (!f) {
		/* Not running: stopped elsewhere (FM, an alarm), unless just
		 * started and not up yet. */
		if (playing && mono_ms() > start_grace_until) {
			playing = false;
			st_url[0] = st_state[0] = st_title[0] = '\0';
			ui_update();
		}
		return;
	}
	if (!playing && mono_ms() > start_grace_until) {
		playing = true;		/* started elsewhere */
		ui_update();
	}
	snprintf(st_state, sizeof(st_state), "%s", state);
	snprintf(st_url, sizeof(st_url), "%s", url);
	snprintf(st_title, sizeof(st_title), "%s", title);
	title_update();
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
/* Station list                                                       */

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

static void list_row_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);

	cur = i;
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

static void list_open(void)
{
	lv_obj_t *row, *l;
	int i;

	list_panel = lv_obj_create(scr);
	lv_obj_remove_style_all(list_panel);
	lv_obj_set_size(list_panel, CHUMBY_APP_W, CHUMBY_APP_H);
	lv_obj_set_style_bg_opa(list_panel, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(list_panel, chumby_color(CHUMBY_COLOR_BG), 0);
	lv_obj_set_flex_flow(list_panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(list_panel, 8, 0);
	lv_obj_set_style_pad_right(list_panel, 8 + CHUMBY_TOUCH_MIN, 0);	/* the list button */
	lv_obj_set_style_pad_row(list_panel, 4, 0);
	lv_obj_set_scroll_dir(list_panel, LV_DIR_VER);
	/* The list button stays on top, to close it. */
	lv_obj_move_foreground(btn_list);

	for (i = 0; i < n_stations; i++) {
		row = lv_button_create(list_panel);
		lv_obj_set_size(row, LV_PCT(100), CHUMBY_TOUCH_MIN + 2);
		lv_obj_set_style_radius(row, 8, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_bg_color(row, chumby_color(i == cur ? CHUMBY_COLOR_ACCENT
								  : CHUMBY_COLOR_SURFACE), 0);
		lv_obj_set_style_text_color(row, chumby_color(i == cur ? CHUMBY_COLOR_BG
								    : CHUMBY_COLOR_TEXT), 0);
		lv_obj_add_event_cb(row, list_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		l = lv_label_create(row);
		lv_label_set_text(l, stations[i].name);
		lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_LARGE), 0);
		lv_obj_set_width(l, LV_PCT(100));
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
	}
	if (cur < n_stations && cur > 2)
		lv_obj_scroll_to_view(lv_obj_get_child(list_panel, cur), LV_ANIM_OFF);
}

static void list_cb(lv_event_t *e)
{
	(void)e;
	if (list_panel)
		list_close();
	else
		list_open();
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
	lv_obj_set_style_text_color(lbl_title, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(art_note, chumby_color(CHUMBY_COLOR_TEXT), 0);
	/* A white logo tile would glare at night. */
	lv_obj_set_style_opa(art_tile, mode == CHUMBY_MODE_NIGHT ? LV_OPA_50 : LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(btn_prev, surface, 0);
	lv_obj_set_style_bg_color(btn_next, surface, 0);
	lv_obj_set_style_bg_color(btn_play, surface, 0);
	lv_obj_set_style_bg_color(btn_play, chumby_color(CHUMBY_COLOR_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(btn_list, surface, LV_STATE_PRESSED);
	lv_obj_set_style_text_color(btn_list, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	if (!lv_obj_is_hidden(art_note))
		art_show_note();	/* gradient in the new palette */
	if (list_panel) {
		list_close();
		list_open();
	}
}

int main(void)
{
	lv_obj_t *content, *middle;

	scr = chumby_app_init("Internet Radio");
	lv_obj_set_scrollable(scr, false);

	/* Top right: the station list. */
	btn_list = round_button(scr, LV_SYMBOL_LIST, CHUMBY_TOUCH_MIN, &lv_font_montserrat_14);
	lv_obj_set_style_bg_opa(btn_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(btn_list, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_align(btn_list, LV_ALIGN_TOP_RIGHT, -4, 0);
	lv_obj_add_event_cb(btn_list, list_cb, LV_EVENT_CLICKED, NULL);

	/* Content: logo on the left, the station's name on the right; what's
	 * playing under both. */
	content = chumby_group(scr, LV_FLEX_FLOW_COLUMN, 8);
	middle = chumby_group(content, LV_FLEX_FLOW_ROW, 16);
	lv_obj_set_width(middle, CHUMBY_APP_W - 24);
	lv_obj_set_style_pad_left(middle, 2, 0);
	art_tile = lv_obj_create(middle);
	lv_obj_remove_style_all(art_tile);
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

	lbl_name = lv_label_create(middle);
	lv_obj_set_style_text_font(lbl_name, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_flex_grow(lbl_name, 1);
	lv_obj_set_style_pad_right(lbl_name, CHUMBY_TOUCH_MIN - 16, 0);
	lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(lbl_name, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(lbl_name, "");

	lbl_title = lv_label_create(content);
	lv_obj_set_style_text_font(lbl_title, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(lbl_title, CHUMBY_APP_W - 24);
	lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(lbl_title, "");
	lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 0);

	/* Bottom: previous station, play/stop, next station. */
	btn_play = round_button(scr, LV_SYMBOL_PLAY, 56, &lv_font_montserrat_28);
	lbl_play = lv_obj_get_child(btn_play, 0);
	lv_obj_align(btn_play, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_add_event_cb(btn_play, play_cb, LV_EVENT_CLICKED, NULL);

	btn_prev = round_button(scr, LV_SYMBOL_PREV, 52, &lv_font_montserrat_28);
	lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_LEFT, 22, 0);
	chumby_center_y_on(btn_prev, btn_play);
	lv_obj_add_event_cb(btn_prev, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

	btn_next = round_button(scr, LV_SYMBOL_NEXT, 52, &lv_font_montserrat_28);
	lv_obj_align(btn_next, LV_ALIGN_BOTTOM_RIGHT, -22, 0);
	chumby_center_y_on(btn_next, btn_play);
	lv_obj_add_event_cb(btn_next, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

	lv_obj_update_layout(scr);
	chumby_center_y_at(content, lv_obj_get_y(btn_play) / 2);

	switch_timer = lv_timer_create(switch_timer_cb, 600, NULL);
	lv_timer_pause(switch_timer);

	load_stations();
	load_current();
	status_read();		/* already playing? which station? */
	if (playing && station_by_url(st_url) >= 0)
		cur = station_by_url(st_url);
	chumby_on_mode_change(on_mode, NULL);
	art_update();
	ui_update();
	lv_timer_create(poll_cb, 500, NULL);

	return chumby_run();
}
