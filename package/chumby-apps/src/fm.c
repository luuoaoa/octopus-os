// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-fm: FM radio, laid out like a "now playing" screen:
 *
 *   header      the station's RDS name (else "FM Radio"), signal bars
 *               (like a phone's reception), scan button
 *   dial        < frequency MHz >   (0.1 MHz steps, hold to repeat)
 *               what's playing (RadioText+ title - artist, else RadioText)
 *               status: only transient messages (scanning, errors)
 *   bottom      previous station, on/off, next station
 *
 * The 1-5 signal bars are relative: from the scan's noise floor to its
 * strongest station. Live while playing (the RDS reader samples it), else
 * the level the station had at the last scan. Scanning is on a long press, so a
 * stray tap can't replace the list.
 *
 * Drives /usr/bin/radio in child processes so the UI never blocks on the
 * ~2.5 s tuner init; the radio script keeps "qn8005 rds" writing RDS and
 * the signal strength to $run_dir/rds while it plays. Volume lives in the
 * shell.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <chumby.h>

#define STATION_FILE	"/var/lib/chumby/radio.station"
#define STATIONS_FILE	"/var/lib/chumby/radio.stations"	/* from "radio scan" */
#define MAX_STATIONS	64
#define RADIO_PIDFILE	"/var/run/radio-loop.pid"
#define FREQ_MIN	876	/* tenths of MHz */
#define FREQ_MAX	1080

enum { REQ_NONE, REQ_ON, REQ_OFF, REQ_TUNE, REQ_SCAN };

struct station {
	int freq;		/* tenths of MHz */
	int rssi;		/* at the last scan */
};

static int freq = 1003;
static bool radio_on;
static int child = -1, child_req, pending = REQ_NONE;
static struct station stations[MAX_STATIONS];
static int n_stations, noise_floor = -1;
static int live_rssi = -1;			/* from the RDS reader */
static lv_obj_t *lbl_title, *lbl_freq, *lbl_unit, *lbl_now, *lbl_status;
#define SIGNAL_BARS	5
static lv_obj_t *signal_bars[SIGNAL_BARS];
static int signal_level;			/* 0 = unknown, else 1..5 */
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

/* The status line is for transient messages only (scanning, errors, hints):
 * on/off shows on the power button. */
static void update_ui(const char *busy)
{
	lv_label_set_text_fmt(lbl_freq, "%d.%d", freq / 10, freq % 10);
	lv_label_set_text(lbl_status, busy ? busy : "");
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
		update_ui("stopping...");
		break;
	case REQ_TUNE:
		argv[1] = "tune";
		argv[2] = f;
		update_ui("tuning...");
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

static void tune_timer_cb(lv_timer_t *t)
{
	(void)t;
	lv_timer_pause(tune_timer);
	save_station();
	if (radio_on)
		request(REQ_TUNE);
}

/* Tune after a short pause, so stepping through doesn't retune each time. */
static void retune_soon(void)
{
	live_rssi = -1;
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

/* RDS name, what's playing and the live signal, from the reader's file. */
static void rds_show(void)
{
	char path[256], line[160], ps[16] = "", rt[80] = "", title[80] = "",
	     artist[80] = "", now[170];
	FILE *f = NULL;
	int rssi = -1;

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
		else if (!strncmp(line, "rssi=", 5))
			rssi = atoi(line + 5);
	}
	if (f)
		fclose(f);
	if (title[0])
		snprintf(now, sizeof(now), "%s%s%s", title, artist[0] ? " - " : "", artist);
	else
		snprintf(now, sizeof(now), "%s", rt);
	/* Only touch the labels on change: no redraw once a second. */
	if (strcmp(lv_label_get_text(lbl_title), ps[0] ? ps : "FM Radio"))
		lv_label_set_text(lbl_title, ps[0] ? ps : "FM Radio");
	if (strcmp(lv_label_get_text(lbl_now), now))
		lv_label_set_text(lbl_now, now);
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

static void on_mode(chumby_mode_t mode, void *user)
{
	lv_color_t surface = chumby_color(CHUMBY_COLOR_SURFACE);

	(void)mode;
	(void)user;
	lv_obj_set_style_text_color(lbl_title, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(lbl_freq, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_unit, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_now, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_status, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	signal_bars_style();
	lv_obj_set_style_bg_color(btn_prev, surface, 0);
	lv_obj_set_style_bg_color(btn_next, surface, 0);
	lv_obj_set_style_bg_color(btn_down, surface, 0);
	lv_obj_set_style_bg_color(btn_up, surface, 0);
	lv_obj_set_style_bg_color(btn_power, surface, 0);
	lv_obj_set_style_bg_color(btn_power, chumby_color(CHUMBY_COLOR_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(btn_scan, surface, LV_STATE_PRESSED);
	lv_obj_set_style_text_color(btn_scan, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
}

int main(void)
{
	lv_obj_t *scr = chumby_app_init("FM Radio"), *dial, *tuner, *dfreq, *bars;
	int i;
	const lv_font_t *xl = chumby_font(CHUMBY_FONT_XL);

	lv_obj_set_scrollable(scr, false);

	/* Header: station name, and a small scan button (hold to scan) with
	 * a full-size touch area. */
	btn_scan = round_button(scr, LV_SYMBOL_REFRESH, CHUMBY_TOUCH_MIN,
				&lv_font_montserrat_14);
	lv_obj_set_style_bg_opa(btn_scan, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(btn_scan, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_align(btn_scan, LV_ALIGN_TOP_RIGHT, -4, 2);
	lv_obj_add_event_cb(btn_scan, scan_cb, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(btn_scan, scan_cb, LV_EVENT_LONG_PRESSED, NULL);
	lbl_title = lv_label_create(scr);
	lv_label_set_text(lbl_title, "FM Radio");
	lv_obj_set_style_text_font(lbl_title, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_width(lbl_title, CHUMBY_APP_W - 12 - CHUMBY_TOUCH_MIN - 4 - 24 - 8);
	lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 12, 0);
	chumby_center_y_on(lbl_title, btn_scan);

	/* Signal: 1-5 bars of rising height, like a phone's reception. */
	bars = lv_obj_create(scr);
	lv_obj_remove_style_all(bars);
	lv_obj_set_size(bars, 5 * 3 + 4 * 2, 14);
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
	lv_obj_align_to(bars, btn_scan, LV_ALIGN_OUT_LEFT_MID, -4, 0);

	/* Dial: < frequency MHz >, now playing, status. */
	dial = chumby_group(scr, LV_FLEX_FLOW_COLUMN, 6);
	tuner = chumby_group(dial, LV_FLEX_FLOW_ROW, 8);
	btn_down = round_button(tuner, "<", CHUMBY_TOUCH_MIN, chumby_font(CHUMBY_FONT_LARGE));
	lv_obj_add_event_cb(btn_down, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
	lv_obj_add_event_cb(btn_down, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
	dfreq = chumby_group(tuner, LV_FLEX_FLOW_ROW, 6);
	lv_obj_set_flex_align(dfreq, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
			      LV_FLEX_ALIGN_END);
	lbl_freq = lv_label_create(dfreq);
	lv_obj_set_style_text_font(lbl_freq, xl, 0);
	/* Room for "108.0", so the < > buttons don't move as it changes. */
	lv_obj_set_width(lbl_freq, 4 * lv_font_get_glyph_width(xl, '0', 0) +
			 lv_font_get_glyph_width(xl, '.', 0));
	lv_obj_set_style_text_align(lbl_freq, LV_TEXT_ALIGN_RIGHT, 0);
	lbl_unit = lv_label_create(dfreq);
	lv_label_set_text(lbl_unit, "MHz");
	/* Bottom-aligned boxes: lift "MHz" by the difference in descent. */
	lv_obj_set_style_pad_bottom(lbl_unit, xl->base_line -
				    chumby_font(CHUMBY_FONT_BODY)->base_line, 0);
	btn_up = round_button(tuner, ">", CHUMBY_TOUCH_MIN, chumby_font(CHUMBY_FONT_LARGE));
	lv_obj_add_event_cb(btn_up, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
	lv_obj_add_event_cb(btn_up, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);

	lbl_now = lv_label_create(dial);
	lv_obj_set_size(lbl_now, CHUMBY_APP_W - 24,
			2 * chumby_font(CHUMBY_FONT_BODY)->line_height);
	lv_label_set_long_mode(lbl_now, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_text_align(lbl_now, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(lbl_now, "");

	lbl_status = lv_label_create(dial);
	lv_obj_set_style_text_font(lbl_status, chumby_font(CHUMBY_FONT_SMALL), 0);
	lv_obj_align(dial, LV_ALIGN_TOP_MID, 0, 0);

	/* Bottom: previous station, power, next station. */
	btn_power = round_button(scr, LV_SYMBOL_POWER, 56, &lv_font_montserrat_28);
	lv_obj_set_checkable(btn_power, true);
	lv_obj_align(btn_power, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_add_event_cb(btn_power, power_cb, LV_EVENT_CLICKED, NULL);

	btn_prev = round_button(scr, LV_SYMBOL_PREV, 52, &lv_font_montserrat_28);
	lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_LEFT, 22, 0);
	chumby_center_y_on(btn_prev, btn_power);
	lv_obj_add_event_cb(btn_prev, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

	btn_next = round_button(scr, LV_SYMBOL_NEXT, 52, &lv_font_montserrat_28);
	lv_obj_align(btn_next, LV_ALIGN_BOTTOM_RIGHT, -22, 0);
	chumby_center_y_on(btn_next, btn_power);
	lv_obj_add_event_cb(btn_next, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

	chumby_center_y_between(dial, btn_scan, btn_power);

	load_station();
	load_stations();
	radio_on = radio_running();
	chumby_on_mode_change(on_mode, NULL);
	update_ui(NULL);

	tune_timer = lv_timer_create(tune_timer_cb, 400, NULL);
	lv_timer_pause(tune_timer);
	lv_timer_create(child_poll, 100, NULL);
	lv_timer_ready(lv_timer_create(rds_timer_cb, 1000, NULL));

	return chumby_run();
}
