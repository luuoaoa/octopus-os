// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-fm: FM radio. Frequency, the station's RDS name and what's
 * playing (RadioText+ title/artist, else RadioText), tune (0.1 MHz,
 * long-press repeat), on/off. Drives /usr/bin/radio in child processes so
 * the UI never blocks on the ~2.5 s tuner init; the radio script keeps
 * "qn8005 rds" writing the RDS state to $run_dir/rds while it plays.
 * Volume lives in the shell.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <chumby.h>

#define STATION_FILE	"/etc/radio.station"
#define RADIO_PIDFILE	"/var/run/radio-loop.pid"
#define FREQ_MIN	876	/* tenths of MHz */
#define FREQ_MAX	1080

enum { REQ_NONE, REQ_ON, REQ_OFF, REQ_TUNE };

static int freq = 1003;
static bool radio_on;
static int child = -1, pending = REQ_NONE;
static lv_obj_t *lbl_freq, *lbl_unit, *lbl_status, *btn_power, *btn_prev, *btn_next;
static lv_obj_t *lbl_station, *lbl_now;
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

static void update_ui(const char *busy)
{
	lv_label_set_text_fmt(lbl_freq, "%d.%d", freq / 10, freq % 10);
	lv_label_set_text(lbl_status, busy ? busy :
			  radio_on ? LV_SYMBOL_AUDIO "  playing" : "off");
	if (radio_on)
		lv_obj_add_state(btn_power, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(btn_power, LV_STATE_CHECKED);
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
	default:
		return;
	}
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
	update_ui(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? NULL : "error");
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

static void step_cb(lv_event_t *e)
{
	freq += (int)(intptr_t)lv_event_get_user_data(e);
	if (freq < FREQ_MIN)
		freq = FREQ_MAX;
	if (freq > FREQ_MAX)
		freq = FREQ_MIN;
	update_ui(NULL);
	lv_timer_reset(tune_timer);
	lv_timer_resume(tune_timer);
}

static void power_cb(lv_event_t *e)
{
	(void)e;
	radio_on = !radio_on;	/* optimistic, corrected when the child exits */
	request(radio_on ? REQ_ON : REQ_OFF);
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *sym, int size)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_obj_center(l);
	return b;
}

/* Station name and now playing from the RDS decoder's key=value file. */
static void rds_show(void)
{
	char path[256], line[160], ps[16] = "", rt[80] = "", title[80] = "",
	     artist[80] = "", now[170];
	FILE *f = NULL;

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
	}
	if (f)
		fclose(f);
	if (title[0])
		snprintf(now, sizeof(now), "%s%s%s", title, artist[0] ? " - " : "", artist);
	else
		snprintf(now, sizeof(now), "%s", rt);
	/* Only touch the labels on change: no redraw once a second. */
	if (strcmp(lv_label_get_text(lbl_station), ps))
		lv_label_set_text(lbl_station, ps);
	if (strcmp(lv_label_get_text(lbl_now), now))
		lv_label_set_text(lbl_now, now);
}

static void rds_timer_cb(lv_timer_t *t)
{
	(void)t;
	rds_show();
}

static void on_mode(chumby_mode_t mode, void *user)
{
	(void)mode;
	(void)user;
	lv_obj_set_style_text_color(lbl_freq, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_unit, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_status, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(lbl_station, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(lbl_now, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_bg_color(btn_prev, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_color(btn_next, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_color(btn_power, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_color(btn_power, chumby_color(CHUMBY_COLOR_ACCENT), LV_STATE_CHECKED);
}

int main(void)
{
	lv_obj_t *scr = chumby_app_init("FM Radio"), *title, *dial, *freq;

	lv_obj_set_scrollable(scr, false);

	title = lv_label_create(scr);
	lv_label_set_text(title, "FM Radio");
	lv_obj_set_style_text_font(title, chumby_font(CHUMBY_FONT_SMALL), 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

	/*
	 * Dial: frequency with "MHz" on its baseline, the RDS station name,
	 * now playing (two lines, kept tall when empty so nothing jumps),
	 * and the status.
	 */
	dial = chumby_group(scr, LV_FLEX_FLOW_COLUMN, 6);
	freq = chumby_group(dial, LV_FLEX_FLOW_ROW, 6);
	lv_obj_set_flex_align(freq, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
			      LV_FLEX_ALIGN_END);
	lbl_freq = lv_label_create(freq);
	lv_obj_set_style_text_font(lbl_freq, chumby_font(CHUMBY_FONT_XL), 0);
	lbl_unit = lv_label_create(freq);
	lv_label_set_text(lbl_unit, "MHz");
	/* Bottom-aligned boxes: lift "MHz" by the difference in descent. */
	lv_obj_set_style_pad_bottom(lbl_unit, chumby_font(CHUMBY_FONT_XL)->base_line -
				    chumby_font(CHUMBY_FONT_BODY)->base_line, 0);
	lbl_station = lv_label_create(dial);
	lv_obj_set_style_text_font(lbl_station, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_label_set_text(lbl_station, "");
	lbl_now = lv_label_create(dial);
	lv_obj_set_size(lbl_now, CHUMBY_APP_W - 24,
			2 * chumby_font(CHUMBY_FONT_BODY)->line_height);
	lv_label_set_long_mode(lbl_now, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_text_align(lbl_now, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(lbl_now, "");
	lbl_status = lv_label_create(dial);
	lv_obj_set_style_text_font(lbl_status, chumby_font(CHUMBY_FONT_SMALL), 0);
	lv_obj_align(dial, LV_ALIGN_TOP_MID, 0, 0);

	btn_prev = round_button(scr, LV_SYMBOL_LEFT, 56);
	lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_LEFT, 18, -20);
	lv_obj_add_event_cb(btn_prev, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
	lv_obj_add_event_cb(btn_prev, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);

	btn_power = round_button(scr, LV_SYMBOL_POWER, 64);
	lv_obj_set_checkable(btn_power, true);
	lv_obj_align(btn_power, LV_ALIGN_BOTTOM_MID, 0, -16);
	lv_obj_add_event_cb(btn_power, power_cb, LV_EVENT_CLICKED, NULL);

	btn_next = round_button(scr, LV_SYMBOL_RIGHT, 56);
	lv_obj_align(btn_next, LV_ALIGN_BOTTOM_RIGHT, -18, -20);
	lv_obj_add_event_cb(btn_next, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
	lv_obj_add_event_cb(btn_next, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);

	chumby_center_y_between(dial, title, btn_power);

	load_station();
	radio_on = radio_running();
	chumby_on_mode_change(on_mode, NULL);
	update_ui(NULL);

	tune_timer = lv_timer_create(tune_timer_cb, 400, NULL);
	lv_timer_pause(tune_timer);
	lv_timer_create(child_poll, 100, NULL);
	lv_timer_ready(lv_timer_create(rds_timer_cb, 1000, NULL));

	return chumby_run();
}
