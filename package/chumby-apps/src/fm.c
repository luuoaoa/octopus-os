// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-fm: FM radio, laid out like an iPod's "now playing":
 *
 *   top right   signal bars (1-5, like a phone), scan (the shell's tab
 *               bar names the app)
 *   middle      station art | frequency, with < > (0.1 MHz) under it
 *   title       station name (RDS, else call sign and city from the FCC
 *               lookup) and what's playing; scrolls when too long
 *   bottom      previous station, on/off, next station
 *
 * Art: chumby-station-art finds the station (FCC, nearest transmitter)
 * and its logo (Wikipedia), cached in /var/lib/chumby/art per frequency;
 * a music note on a gradient tile when there is none.
 *
 * The signal bars are relative, from the last scan's noise floor to its
 * strongest station: live while playing (the RDS reader samples it), else
 * the level the station had at the last scan. Scanning is on a long press,
 * so a stray tap can't replace the list.
 *
 * Drives /usr/bin/radio and chumby-station-art in child processes, so the
 * UI never blocks (tuner init ~2.5 s, lookups need the network). Volume
 * lives in the shell.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <chumby.h>

#define STATION_FILE	"/var/lib/chumby/radio.station"
#define STATIONS_FILE	"/var/lib/chumby/radio.stations"	/* from "radio scan" */
#define ART_DIR		"/var/lib/chumby/art"
#define ART_TOOL	"/usr/bin/chumby-station-art"
#define MAX_STATIONS	64
#define RADIO_PIDFILE	"/var/run/radio-loop.pid"
#define FREQ_MIN	876	/* tenths of MHz */
#define FREQ_MAX	1080
#define ART_SIZE	92	/* the tile */
#define ART_PAD		8	/* logo inset on the tile */
#define SIGNAL_BARS	5

enum { REQ_NONE, REQ_ON, REQ_OFF, REQ_TUNE, REQ_SCAN };

struct station {
	int freq;		/* tenths of MHz */
	int rssi;		/* at the last scan */
};

static int freq = 1003;
static bool radio_on;
static int child = -1, child_req, pending = REQ_NONE;
static int art_child = -1, art_freq;		/* lookup running, for which freq */
static struct station stations[MAX_STATIONS];
static int n_stations, noise_floor = -1;
static int live_rssi = -1;			/* from the RDS reader */
static int signal_level;			/* 0 = unknown, else 1..5 */
static char rds_name[16], rds_now[170];		/* from the RDS reader */
static char call_name[48];			/* from the art lookup */
static char message[64];			/* transient: scanning, errors */
static lv_obj_t *signal_bars[SIGNAL_BARS];
static lv_obj_t *lbl_freq, *lbl_title;
static lv_obj_t *art_tile, *art_img, *art_note;
static lv_obj_t *btn_power, *btn_prev, *btn_next, *btn_down, *btn_up, *btn_scan;
static lv_timer_t *tune_timer;

static bool radio_running(void)
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

static void load_station(void)
{
	FILE *f = fopen(STATION_FILE, "r");
	double mhz;

	if (f && fscanf(f, "%lf", &mhz) == 1 && mhz >= 76 && mhz <= 108)
		freq = (int)(mhz * 10 + 0.5);
	if (f)
		fclose(f);
}

static void save_station(void)
{
	FILE *f = fopen(STATION_FILE, "w");

	if (f) {
		fprintf(f, "%d.%d\n", freq / 10, freq % 10);
		fclose(f);
	}
}

static int cmp_station(const void *a, const void *b)
{
	return ((const struct station *)a)->freq - ((const struct station *)b)->freq;
}

/* "#floor N", then "MHz rssi" lines (rssi optional: older lists). */
static void load_stations(void)
{
	FILE *f = fopen(STATIONS_FILE, "r");
	char line[64];
	double mhz;
	int rssi;

	n_stations = 0;
	noise_floor = -1;
	while (f && n_stations < MAX_STATIONS && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "#floor %d", &rssi) == 1) {
			noise_floor = rssi;
			continue;
		}
		if (sscanf(line, "%lf", &mhz) != 1 || mhz < 76 || mhz > 108)
			continue;
		if (sscanf(line, "%*f %d", &rssi) != 1)
			rssi = -1;
		stations[n_stations].freq = (int)(mhz * 10 + 0.5);
		stations[n_stations].rssi = rssi;
		n_stations++;
	}
	if (f)
		fclose(f);
	qsort(stations, n_stations, sizeof(stations[0]), cmp_station);
}

static int station_index(void)
{
	int i;

	for (i = 0; i < n_stations; i++)
		if (stations[i].freq == freq)
			return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Signal bars                                                        */

/* 0..100 between the scan's noise floor and its strongest station. */
static int signal_pct(int rssi)
{
	int lo = noise_floor >= 0 ? noise_floor : 25, hi = lo + 30, i, pct;

	for (i = 0; i < n_stations; i++)
		if (stations[i].rssi > hi)
			hi = stations[i].rssi;
	pct = (rssi - lo) * 100 / (hi - lo);
	return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

/* Lit bars in the text colour, the rest dim; all dim when unknown. */
static void signal_bars_style(void)
{
	int i;

	for (i = 0; i < SIGNAL_BARS; i++) {
		bool lit = i < signal_level;

		lv_obj_set_style_bg_color(signal_bars[i], chumby_color(lit ?
					  CHUMBY_COLOR_TEXT : CHUMBY_COLOR_TEXT_DIM), 0);
		lv_obj_set_style_bg_opa(signal_bars[i], lit ? LV_OPA_COVER : LV_OPA_40, 0);
	}
}

static void signal_update(void)
{
	int i = station_index(), rssi = -1, level;

	if (radio_on && live_rssi >= 0)
		rssi = live_rssi;
	else if (i >= 0)
		rssi = stations[i].rssi;
	level = rssi < 0 ? 0 : 1 + signal_pct(rssi) * (SIGNAL_BARS - 1) / 100;
	if (level != signal_level) {
		signal_level = level;
		signal_bars_style();
	}
}

/* ------------------------------------------------------------------ */
/* Title line and art                                                 */

/* Station name (RDS, else call sign) and what's playing; or a transient
 * message. Scrolls (label long mode) when it doesn't fit. */
static void title_update(void)
{
	char text[256];
	const char *name = rds_name[0] ? rds_name : call_name;

	if (message[0])
		snprintf(text, sizeof(text), "%s", message);
	else if (name[0] && rds_now[0])
		snprintf(text, sizeof(text), "%s  |  %s", name, rds_now);
	else if (name[0] || rds_now[0])
		snprintf(text, sizeof(text), "%s", name[0] ? name : rds_now);
	else
		snprintf(text, sizeof(text), "%d.%d FM", freq / 10, freq % 10);
	if (strcmp(lv_label_get_text(lbl_title), text))
		lv_label_set_text(lbl_title, text);
}

static void art_show_note(void)
{
	lv_obj_set_hidden(art_img, true);
	lv_obj_set_hidden(art_note, false);
	lv_obj_set_style_bg_color(art_tile, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_bg_grad_color(art_tile, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_VER, 0);
}

static void art_show_logo(const char *path)
{
	if (!chumby_logo_set(art_img, path, ART_SIZE - 2 * ART_PAD)) {
		art_show_note();
		return;
	}
	lv_obj_center(art_img);
	lv_obj_set_hidden(art_img, false);
	lv_obj_set_hidden(art_note, true);
	/* Logos are made for a white background, like album art. */
	lv_obj_set_style_bg_color(art_tile, lv_color_white(), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_NONE, 0);
}

/* The lookup result for the current frequency: show it, and say whether
 * a (new) lookup is due. */
static bool art_load(void)
{
	char path[96], line[200], call[24] = "", city[40] = "", logo[128] = "";
	long long retry = 0;
	FILE *f;

	snprintf(path, sizeof(path), ART_DIR "/%d.%d.info", freq / 10, freq % 10);
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "call=", 5))
			snprintf(call, sizeof(call), "%.23s", line + 5);
		else if (!strncmp(line, "city=", 5))
			snprintf(city, sizeof(city), "%.39s", line + 5);
		else if (!strncmp(line, "logo=", 5))
			snprintf(logo, sizeof(logo), "%.127s", line + 5);
		else if (!strncmp(line, "retry=", 6))
			retry = atoll(line + 6);
	}
	if (f)
		fclose(f);
	if (call[0] && city[0])
		snprintf(call_name, sizeof(call_name), "%s, %s", call, city);
	else
		snprintf(call_name, sizeof(call_name), "%s", call);
	if (logo[0])
		art_show_logo(logo);
	else
		art_show_note();
	title_update();
	return !f || (retry && time(NULL) >= retry);
}

/* Look the station up in the background, once the frequency has settled. */
static void art_refresh(void)
{
	char mhz[16];
	char *argv[] = { "chumby-station-art", mhz, NULL };

	if (!art_load() || art_child > 0)
		return;
	snprintf(mhz, sizeof(mhz), "%d.%d", freq / 10, freq % 10);
	art_freq = freq;
	art_child = chumby_spawn(ART_TOOL, argv);
}

/* ------------------------------------------------------------------ */
/* Radio control                                                      */

static void update_ui(const char *msg)
{
	lv_label_set_text_fmt(lbl_freq, "%d.%d", freq / 10, freq % 10);
	snprintf(message, sizeof(message), "%s", msg ? msg : "");
	title_update();
	if (radio_on)
		lv_obj_add_state(btn_power, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(btn_power, LV_STATE_CHECKED);
	signal_update();
}

static void run_request(int req)
{
	char f[16];
	char *argv[4] = { "radio", NULL, NULL, NULL };

	snprintf(f, sizeof(f), "%d.%d", freq / 10, freq % 10);
	switch (req) {
	case REQ_ON:
		argv[1] = "on";
		argv[2] = f;
		update_ui("starting...");
		break;
	case REQ_OFF:
		argv[1] = "off";
		update_ui(NULL);
		break;
	case REQ_TUNE:
		argv[1] = "tune";
		argv[2] = f;
		update_ui(NULL);
		break;
	case REQ_SCAN:
		argv[1] = "scan";
		update_ui(LV_SYMBOL_REFRESH "  scanning...");
		break;
	default:
		return;
	}
	child_req = req;
	child = chumby_spawn("/usr/bin/radio", argv);
	if (child < 0)
		update_ui("error: no /usr/bin/radio");
}

static void request(int req)
{
	if (child > 0)
		pending = req;	/* latest request wins, run when idle */
	else
		run_request(req);
}

static void child_poll(lv_timer_t *t)
{
	int status;

	(void)t;
	if (art_child > 0 && waitpid(art_child, &status, WNOHANG) == art_child) {
		art_child = -1;
		if (art_freq == freq)
			art_load();
		else
			art_refresh();	/* moved on meanwhile */
	}
	if (child <= 0 || waitpid(child, &status, WNOHANG) != child)
		return;
	child = -1;
	radio_on = radio_running();
	live_rssi = -1;		/* a new station: wait for a fresh reading */
	if (child_req == REQ_SCAN && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		char msg[48];

		load_stations();
		snprintf(msg, sizeof(msg), n_stations == 1 ? "1 station found" :
			 "%d stations found", n_stations);
		update_ui(msg);
	} else {
		update_ui(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? NULL : "error");
	}
	if (pending != REQ_NONE) {
		int r = pending;

		pending = REQ_NONE;
		run_request(r);
	}
}

/* The frequency settled: tune, and look its station up. */
static void tune_timer_cb(lv_timer_t *t)
{
	(void)t;
	lv_timer_pause(tune_timer);
	save_station();
	if (radio_on)
		request(REQ_TUNE);
	art_refresh();
}

/* Tune after a short pause, so stepping through doesn't retune each time. */
static void retune_soon(void)
{
	live_rssi = -1;
	rds_name[0] = rds_now[0] = call_name[0] = '\0';
	art_show_note();
	update_ui(NULL);
	lv_timer_reset(tune_timer);
	lv_timer_resume(tune_timer);
}

static void step_cb(lv_event_t *e)
{
	freq += (int)(intptr_t)lv_event_get_user_data(e);
	if (freq < FREQ_MIN)
		freq = FREQ_MAX;
	if (freq > FREQ_MAX)
		freq = FREQ_MIN;
	retune_soon();
}

/* Previous / next station from the last scan, wrapping around. */
static void station_cb(lv_event_t *e)
{
	int dir = (int)(intptr_t)lv_event_get_user_data(e), i;

	if (!n_stations) {
		update_ui("no stations: hold " LV_SYMBOL_REFRESH " to scan");
		return;
	}
	if (dir > 0) {
		for (i = 0; i < n_stations && stations[i].freq <= freq; i++)
			;
		freq = stations[i < n_stations ? i : 0].freq;
	} else {
		for (i = n_stations - 1; i >= 0 && stations[i].freq >= freq; i--)
			;
		freq = stations[i >= 0 ? i : n_stations - 1].freq;
	}
	retune_soon();
}

static void scan_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
		request(REQ_SCAN);
	else
		update_ui("hold " LV_SYMBOL_REFRESH " to scan");
}

static void power_cb(lv_event_t *e)
{
	(void)e;
	radio_on = !radio_on;	/* optimistic, corrected when the child exits */
	request(radio_on ? REQ_ON : REQ_OFF);
}

/* RDS name, what's playing and the live signal, from the reader's file. */
static void rds_show(void)
{
	char path[256], line[200], ps[16] = "", rt[80] = "", title[80] = "",
	     artist[80] = "", pstext[164] = "", now[170];
	FILE *f = NULL;
	int rssi = -1, psdyn = 0;

	if (radio_on) {
		snprintf(path, sizeof(path), "%s/rds", chumby_run_dir());
		f = fopen(path, "r");
	}
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "ps=", 3))
			snprintf(ps, sizeof(ps), "%.15s", line + 3);
		else if (!strncmp(line, "rt=", 3))
			snprintf(rt, sizeof(rt), "%.79s", line + 3);
		else if (!strncmp(line, "title=", 6))
			snprintf(title, sizeof(title), "%.79s", line + 6);
		else if (!strncmp(line, "artist=", 7))
			snprintf(artist, sizeof(artist), "%.79s", line + 7);
		else if (!strncmp(line, "psdyn=", 6))
			psdyn = atoi(line + 6);
		else if (!strncmp(line, "pstext=", 7))
			snprintf(pstext, sizeof(pstext), "%.163s", line + 7);
		else if (!strncmp(line, "rssi=", 5))
			rssi = atoi(line + 5);
	}
	if (f)
		fclose(f);
	/* What's playing: RadioText+ tags, else the RadioText, else the
	 * words a station scrolls through its PS. A scrolling PS isn't the
	 * station's name: the call sign (from the lookup) is used then. */
	if (title[0])
		snprintf(now, sizeof(now), "%s%s%s", title, artist[0] ? " - " : "", artist);
	else if (rt[0])
		snprintf(now, sizeof(now), "%s", rt);
	else
		snprintf(now, sizeof(now), "%s", psdyn ? pstext : "");
	if (psdyn)
		ps[0] = '\0';
	if (strcmp(ps, rds_name) || strcmp(now, rds_now)) {
		snprintf(rds_name, sizeof(rds_name), "%s", ps);
		snprintf(rds_now, sizeof(rds_now), "%s", now);
		title_update();
	}
	if (rssi != live_rssi) {
		live_rssi = rssi;
		signal_update();
	}
}

static void rds_timer_cb(lv_timer_t *t)
{
	(void)t;
	rds_show();
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
	lv_obj_set_style_text_color(lbl_freq, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_title, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(art_note, chumby_color(CHUMBY_COLOR_TEXT), 0);
	/* A white logo tile would glare at night. */
	lv_obj_set_style_opa(art_tile, mode == CHUMBY_MODE_NIGHT ? LV_OPA_50 : LV_OPA_COVER, 0);
	signal_bars_style();
	lv_obj_set_style_bg_color(btn_prev, surface, 0);
	lv_obj_set_style_bg_color(btn_next, surface, 0);
	lv_obj_set_style_bg_color(btn_down, surface, 0);
	lv_obj_set_style_bg_color(btn_up, surface, 0);
	lv_obj_set_style_bg_color(btn_power, surface, 0);
	lv_obj_set_style_bg_color(btn_power, chumby_color(CHUMBY_COLOR_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(btn_scan, surface, LV_STATE_PRESSED);
	lv_obj_set_style_text_color(btn_scan, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	if (!lv_obj_is_hidden(art_note))
		art_show_note();	/* gradient in the new palette */
}

int main(void)
{
	lv_obj_t *scr = chumby_app_init("FM Radio"), *bars, *content, *middle, *right, *steps;
	const lv_font_t *xl = chumby_font(CHUMBY_FONT_XL);
	int i;

	lv_obj_set_scrollable(scr, false);

	/* Top right: a small scan button (hold to scan) with a full-size
	 * touch area, the signal bars under it. */
	btn_scan = round_button(scr, LV_SYMBOL_REFRESH, CHUMBY_TOUCH_MIN,
				&lv_font_montserrat_14);
	lv_obj_set_style_bg_opa(btn_scan, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(btn_scan, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_align(btn_scan, LV_ALIGN_TOP_RIGHT, -4, 0);
	lv_obj_add_event_cb(btn_scan, scan_cb, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(btn_scan, scan_cb, LV_EVENT_LONG_PRESSED, NULL);

	bars = lv_obj_create(scr);
	lv_obj_remove_style_all(bars);
	lv_obj_set_size(bars, SIGNAL_BARS * 3 + (SIGNAL_BARS - 1) * 2, 14);
	lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(bars, 2, 0);
	lv_obj_set_clickable(bars, false);
	for (i = 0; i < SIGNAL_BARS; i++) {
		signal_bars[i] = lv_obj_create(bars);
		lv_obj_remove_style_all(signal_bars[i]);
		lv_obj_set_size(signal_bars[i], 3, 4 + i * 10 / (SIGNAL_BARS - 1));
		lv_obj_set_style_radius(signal_bars[i], 1, 0);
	}
	lv_obj_align_to(bars, btn_scan, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

	/* Content: art on the left, frequency and < > on the right; the
	 * title under both. */
	content = chumby_group(scr, LV_FLEX_FLOW_COLUMN, 8);
	middle = chumby_group(content, LV_FLEX_FLOW_ROW, 16);
	/* Art at the left edge, the frequency centred in the rest (clear of
	 * the scan button). */
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

	right = chumby_group(middle, LV_FLEX_FLOW_COLUMN, 8);
	lv_obj_set_flex_grow(right, 1);
	lv_obj_set_style_pad_right(right, CHUMBY_TOUCH_MIN - 16, 0);
	lbl_freq = lv_label_create(right);
	lv_obj_set_style_text_font(lbl_freq, xl, 0);
	/* Room for "108.0", so the layout doesn't move as it changes. */
	lv_obj_set_width(lbl_freq, 4 * lv_font_get_glyph_width(xl, '0', 0) +
			 lv_font_get_glyph_width(xl, '.', 0));
	lv_obj_set_style_text_align(lbl_freq, LV_TEXT_ALIGN_CENTER, 0);
	steps = chumby_group(right, LV_FLEX_FLOW_ROW, 12);
	btn_down = round_button(steps, "<", CHUMBY_TOUCH_MIN, chumby_font(CHUMBY_FONT_LARGE));
	lv_obj_add_event_cb(btn_down, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
	lv_obj_add_event_cb(btn_down, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
	btn_up = round_button(steps, ">", CHUMBY_TOUCH_MIN, chumby_font(CHUMBY_FONT_LARGE));
	lv_obj_add_event_cb(btn_up, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
	lv_obj_add_event_cb(btn_up, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);

	/* Title: the full width, scrolling smoothly when too long. */
	lbl_title = lv_label_create(content);
	lv_obj_set_style_text_font(lbl_title, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(lbl_title, CHUMBY_APP_W - 24);
	lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(lbl_title, "");
	lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 0);

	/* Bottom: previous station, power, next station. */
	btn_power = round_button(scr, LV_SYMBOL_POWER, 56, &lv_font_montserrat_28);
	lv_obj_set_checkable(btn_power, true);
	lv_obj_align(btn_power, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_add_event_cb(btn_power, power_cb, LV_EVENT_CLICKED, NULL);

	btn_prev = round_button(scr, LV_SYMBOL_PREV, 52, &lv_font_montserrat_28);
	lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_LEFT, 22, 0);
	chumby_center_y_on(btn_prev, btn_power);
	lv_obj_add_event_cb(btn_prev, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

	btn_next = round_button(scr, LV_SYMBOL_NEXT, 52, &lv_font_montserrat_28);
	lv_obj_align(btn_next, LV_ALIGN_BOTTOM_RIGHT, -22, 0);
	chumby_center_y_on(btn_next, btn_power);
	lv_obj_add_event_cb(btn_next, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

	/* Centred in the space above the buttons. */
	lv_obj_update_layout(scr);
	chumby_center_y_at(content, lv_obj_get_y(btn_power) / 2);

	load_station();
	load_stations();
	radio_on = radio_running();
	chumby_on_mode_change(on_mode, NULL);
	update_ui(NULL);
	art_refresh();

	tune_timer = lv_timer_create(tune_timer_cb, 400, NULL);
	lv_timer_pause(tune_timer);
	lv_timer_create(child_poll, 100, NULL);
	lv_timer_ready(lv_timer_create(rds_timer_cb, 1000, NULL));

	return chumby_run();
}
