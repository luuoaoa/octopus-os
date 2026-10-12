// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * octopus-clock: clock and alarms.
 *
 * Views: the clock (HH:MM, date, next alarm), the alarm list, the alarm
 * editor, and the ringing screen on top of everything. Alarms are stored
 * by liboctopus in /etc/octopus/alarms.conf and rung by the shell, so they
 * go off even when this app isn't running; while one rings the shell
 * brings this app up and we show Snooze / Stop (the snooze button on top
 * of the case snoozes too).
 *
 * Every control is at least OCTOPUS_TOUCH_MIN in both directions: it's a
 * resistive touchscreen.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <octopus.h>

#define GAP	octopus_px(8)	/* sizes: 320x240's, scaled for the screen */
#define BAR_H	OCTOPUS_TOUCH_MIN

static lv_obj_t *scr;
static lv_obj_t *view_clock, *view_list, *view_edit, *view_ring;

static struct octopus_alarm alarms[OCTOPUS_MAX_ALARMS];
static int n_alarms;

static octopus_alarm_state_t alarm_state = -1;
static time_t snooze_until;
static long shown_minute = -1;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */

static lv_obj_t *view_create(void)
{
	lv_obj_t *v = lv_obj_create(scr);

	lv_obj_remove_style_all(v);
	lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
	lv_obj_set_scrollable(v, false);
	lv_obj_set_hidden(v, true);
	return v;
}

static void view_show(lv_obj_t *v)
{
	lv_obj_set_hidden(view_clock, true);
	lv_obj_set_hidden(view_list, true);
	lv_obj_set_hidden(view_edit, true);
	lv_obj_set_hidden(v, false);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, const lv_font_t *font,
			int w, int h, lv_event_cb_t cb, void *user)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, w, h);
	lv_obj_set_style_radius(b, octopus_px(12), 0);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_center(l);
	if (cb)
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
	return b;
}

static lv_obj_t *label(lv_obj_t *parent, octopus_font_t font, octopus_color_t color)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, octopus_font(font), 0);
	lv_obj_set_style_text_color(l, octopus_color(color), 0);
	return l;
}

static void alarms_load(void)
{
	n_alarms = octopus_alarms_load(alarms, OCTOPUS_MAX_ALARMS);
}

static void alarms_save(void)
{
	if (!octopus_alarms_save(alarms, n_alarms))
		LV_LOG_WARN("cannot save %s", octopus_alarms_path());
}

/* ------------------------------------------------------------------ */
/* Clock view                                                         */

static lv_obj_t *face, *lbl_time, *lbl_date, *lbl_next, *ic_next, *ic_offline;

/* Red: legible on the day and the night background alike. */
#define WARN_COLOR	lv_color_hex(0xe03a2f)

static void list_show(void);

/* The face is centred in the space above the alarm pill. */
static void face_center(void)
{
	octopus_center_y_at(face, (OCTOPUS_APP_H - GAP - OCTOPUS_TOUCH_MIN) / 2);
}

/* The date as big as it goes: the long form if it fits the width, else
 * shortened ("Wednesday, 24 September" -> "Wed, 24 September" -> "Wed,
 * 24 Sep"). */
static void date_set(const struct tm *tm)
{
	static const char *const formats[] = { "%A, %d %B", "%a, %d %B", "%a, %d %b" };
	char buf[64];
	unsigned i;

	for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		strftime(buf, sizeof(buf), formats[i], tm);
		lv_label_set_text(lbl_date, buf);
		lv_obj_update_layout(lbl_date);
		if (lv_obj_get_width(lbl_date) <= OCTOPUS_APP_W - 2 * GAP)
			break;
	}
}

static void clock_update(void)
{
	char buf[64];
	time_t now = time(NULL), next = 0;
	struct tm tm;
	int i;

	localtime_r(&now, &tm);
	if (octopus_time_valid()) {
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		lv_label_set_text(lbl_time, buf);
		date_set(&tm);
	} else {
		lv_label_set_text(lbl_time, "--:--");
		lv_label_set_text(lbl_date, "");
	}
	/* Not set from the internet this boot: the time is the RTC's guess
	 * (or none), and the shell holds the alarms until it is. */
	lv_obj_set_hidden(ic_offline, octopus_time_synced());

	for (i = 0; i < n_alarms; i++) {
		time_t t = octopus_alarm_next(&alarms[i], now);

		if (t && (!next || t < next))
			next = t;
	}
	if (alarm_state == OCTOPUS_ALARM_SNOOZED) {
		localtime_r(&snooze_until, &tm);
		strftime(buf, sizeof(buf), "Snoozed until %H:%M", &tm);
	} else if (next) {
		localtime_r(&next, &tm);
		strftime(buf, sizeof(buf), next - now < 24 * 3600 ? "Alarm %H:%M" :
			 "Alarm %a %H:%M", &tm);
	} else {
		snprintf(buf, sizeof(buf), "No alarm set");
	}
	lv_label_set_text(lbl_next, buf);
	lv_obj_set_style_text_color(ic_next, octopus_color((next || alarm_state ==
				    OCTOPUS_ALARM_SNOOZED) && octopus_time_synced() ?
				    OCTOPUS_COLOR_ACCENT : OCTOPUS_COLOR_TEXT_DIM), 0);
}

static void alarms_btn_cb(lv_event_t *e)
{
	(void)e;
	list_show();
}

static void build_clock(void)
{
	lv_obj_t *b;

	face = octopus_group(view_clock, LV_FLEX_FLOW_COLUMN, octopus_px(2));
	lbl_time = label(face, OCTOPUS_FONT_HUGE, OCTOPUS_COLOR_TEXT);
	lbl_date = label(face, OCTOPUS_FONT_MEDIUM, OCTOPUS_COLOR_TEXT_DIM);
	lv_obj_align(face, LV_ALIGN_TOP_MID, 0, 0);

	/* No network time yet: a red icon in the corner. */
	ic_offline = lv_label_create(view_clock);
	lv_obj_set_style_text_font(ic_offline, octopus_icon_font(20), 0);
	lv_obj_set_style_text_color(ic_offline, WARN_COLOR, 0);
	lv_label_set_text(ic_offline, OCTOPUS_ICON_NO_NETWORK);
	lv_obj_align(ic_offline, LV_ALIGN_TOP_RIGHT, -GAP, GAP);

	/* A small pill at the bottom opens the alarms. */
	b = lv_button_create(view_clock);
	lv_obj_set_size(b, LV_SIZE_CONTENT, OCTOPUS_TOUCH_MIN);
	lv_obj_set_style_pad_hor(b, octopus_px(16), 0);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(b, octopus_px(8), 0);
	lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -GAP);
	lv_obj_add_event_cb(b, alarms_btn_cb, LV_EVENT_CLICKED, NULL);
	ic_next = lv_label_create(b);
	lv_label_set_text(ic_next, LV_SYMBOL_BELL);
	lv_obj_set_style_text_font(ic_next, octopus_symbol_font(14), 0);
	lbl_next = lv_label_create(b);
	lv_obj_set_style_text_font(lbl_next, octopus_font(OCTOPUS_FONT_SMALL), 0);
	face_center();
}

/* ------------------------------------------------------------------ */
/* Alarm editor                                                       */

static int edit_index;		/* -1 = new */
static struct octopus_alarm edit;
static lv_obj_t *lbl_hour, *lbl_min, *lbl_days, *btn_delete;

static const unsigned char day_presets[] = {
	0, OCTOPUS_DAYS_ALL, OCTOPUS_DAYS_WEEKDAYS, OCTOPUS_DAYS_WEEKEND,
};

static void edit_update(void)
{
	char buf[48];

	lv_label_set_text_fmt(lbl_hour, "%02d", edit.time / 60);
	lv_label_set_text_fmt(lbl_min, "%02d", edit.time % 60);
	lv_label_set_text_fmt(lbl_days, "Repeat:  %s",
			      octopus_alarm_days_text(edit.days, buf, sizeof(buf)));
}

static void edit_show(int index)
{
	edit_index = index;
	if (index >= 0) {
		edit = alarms[index];
	} else {
		edit.time = 7 * 60;
		edit.days = OCTOPUS_DAYS_WEEKDAYS;
	}
	edit.enabled = true;	/* editing an alarm means you want it */
	if (index >= 0)
		lv_obj_set_hidden(btn_delete, false);
	else
		lv_obj_set_hidden(btn_delete, true);
	edit_update();
	view_show(view_edit);
}

/* Hour: +-1. Minute: +-1 per tap, holding steps 5 (on multiples of 5). */
static void step_cb(lv_event_t *e)
{
	int which = (int)(intptr_t)lv_event_get_user_data(e);	/* +-1 h, +-2 min */
	bool repeat = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED_REPEAT;
	int h = edit.time / 60, m = edit.time % 60;

	if (which == 1 || which == -1) {
		h = (h + which + 24) % 24;
	} else {
		int dir = which > 0 ? 1 : -1;

		if (repeat)
			m = dir > 0 ? (m / 5 + 1) * 5 : ((m + 4) / 5 - 1) * 5;
		else
			m += dir;
		m = (m + 60) % 60;
	}
	edit.time = h * 60 + m;
	edit_update();
}

static void days_cb(lv_event_t *e)
{
	size_t i, n = sizeof(day_presets);

	(void)e;
	for (i = 0; i < n && day_presets[i] != edit.days; i++)
		;
	/* A custom set (from the file) moves on to the first preset. */
	edit.days = day_presets[i < n ? (i + 1) % n : 0];
	edit_update();
}

static void edit_cancel_cb(lv_event_t *e)
{
	(void)e;
	list_show();
}

static void edit_save_cb(lv_event_t *e)
{
	(void)e;
	if (edit_index >= 0)
		alarms[edit_index] = edit;
	else if (n_alarms < OCTOPUS_MAX_ALARMS)
		alarms[n_alarms++] = edit;
	alarms_save();
	list_show();
}

static void edit_delete_cb(lv_event_t *e)
{
	(void)e;
	if (edit_index >= 0 && edit_index < n_alarms) {
		memmove(&alarms[edit_index], &alarms[edit_index + 1],
			(n_alarms - edit_index - 1) * sizeof(alarms[0]));
		n_alarms--;
		alarms_save();
	}
	list_show();
}

/*
 * In the app's 208 px: the buttons along the top, then the time with each
 * number's + over - beside it (outside: [+-] HH : MM [+-]), then the days.
 */
#define STEP_W		octopus_px(56)
#define DIGITS_W	octopus_px(72)
#define COLON_W		octopus_px(28)
#define STEP_Y		octopus_px(54)		/* under the top row */
#define DAYS_H		octopus_px(44)
#define TOP_W		octopus_px(56)		/* the buttons along the top */
#define TOP_Y		octopus_px(4)

static lv_obj_t *step_button(const char *sym, int which, int x, int y)
{
	lv_obj_t *b = button(view_edit, sym, octopus_symbol_font(28), STEP_W, BAR_H,
			     step_cb, (void *)(intptr_t)which);

	lv_obj_add_event_cb(b, step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
			    (void *)(intptr_t)which);
	lv_obj_set_pos(b, x, y);
	return b;
}

static void build_edit(void)
{
	lv_obj_t *b, *colon, *h_up, *h_down, *m_up, *m_down;
	const int row_w = 2 * (STEP_W + octopus_px(6) + DIGITS_W) + COLON_W;
	const int x0 = (OCTOPUS_APP_W - row_w) / 2;
	const int hour_x = x0 + STEP_W + octopus_px(6), min_x = hour_x + DIGITS_W + COLON_W;
	const int down_y = STEP_Y + BAR_H + octopus_px(4);
	const int days_y = down_y + BAR_H + octopus_px(10);

	b = button(view_edit, LV_SYMBOL_CLOSE, octopus_symbol_font(28), TOP_W, BAR_H,
		   edit_cancel_cb, NULL);
	lv_obj_set_pos(b, GAP, TOP_Y);
	btn_delete = button(view_edit, LV_SYMBOL_TRASH, octopus_symbol_font(28), TOP_W, BAR_H,
			    edit_delete_cb, NULL);
	lv_obj_set_pos(btn_delete, GAP + TOP_W + GAP, TOP_Y);
	b = button(view_edit, LV_SYMBOL_OK "  Save", octopus_font(OCTOPUS_FONT_BODY),
		   octopus_px(96), BAR_H, edit_save_cb, NULL);
	lv_obj_set_style_bg_color(b, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
	lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -GAP, TOP_Y);

	h_up = step_button(LV_SYMBOL_PLUS, 1, x0, STEP_Y);
	h_down = step_button(LV_SYMBOL_MINUS, -1, x0, down_y);
	m_up = step_button(LV_SYMBOL_PLUS, 2, min_x + DIGITS_W + 6, STEP_Y);
	m_down = step_button(LV_SYMBOL_MINUS, -2, min_x + DIGITS_W + 6, down_y);

	lbl_hour = label(view_edit, OCTOPUS_FONT_XL, OCTOPUS_COLOR_TEXT);
	lv_obj_set_width(lbl_hour, DIGITS_W);
	lv_obj_set_style_text_align(lbl_hour, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(lbl_hour, hour_x, 0);
	octopus_center_y_between(lbl_hour, h_up, h_down);
	lbl_min = label(view_edit, OCTOPUS_FONT_XL, OCTOPUS_COLOR_TEXT);
	lv_obj_set_width(lbl_min, DIGITS_W);
	lv_obj_set_style_text_align(lbl_min, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(lbl_min, min_x, 0);
	octopus_center_y_between(lbl_min, m_up, m_down);
	colon = label(view_edit, OCTOPUS_FONT_XL, OCTOPUS_COLOR_TEXT_DIM);
	lv_label_set_text(colon, ":");
	lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 0);
	octopus_center_y_between(colon, h_up, h_down);

	b = lv_button_create(view_edit);
	lv_obj_set_size(b, row_w, DAYS_H);
	lv_obj_set_style_radius(b, octopus_px(12), 0);
	lv_obj_set_pos(b, x0, days_y);
	lv_obj_add_event_cb(b, days_cb, LV_EVENT_CLICKED, NULL);
	lbl_days = lv_label_create(b);
	lv_obj_center(lbl_days);
}

/* ------------------------------------------------------------------ */
/* Alarm list                                                         */

static lv_obj_t *list, *btn_add;

static void list_back_cb(lv_event_t *e)
{
	(void)e;
	clock_update();
	view_show(view_clock);
}

static void list_add_cb(lv_event_t *e)
{
	(void)e;
	edit_show(-1);
}

static void row_edit_cb(lv_event_t *e)
{
	edit_show((int)(intptr_t)lv_event_get_user_data(e));
}

static void list_rebuild(void);

static void row_toggle_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);

	alarms[i].enabled = !alarms[i].enabled;
	alarms_save();
	list_rebuild();
}

static void list_rebuild(void)
{
	char buf[48];
	int i;

	lv_obj_clean(list);
	for (i = 0; i < n_alarms; i++) {
		const struct octopus_alarm *a = &alarms[i];
		lv_obj_t *row = lv_button_create(list), *text, *t, *d, *sw;

		/* Row: tap to edit; the On/Off button on the right toggles. */
		lv_obj_set_size(row, OCTOPUS_APP_W - 2 * GAP, octopus_px(56));
		lv_obj_set_style_radius(row, octopus_px(12), 0);
		lv_obj_set_style_pad_all(row, 0, 0);
		lv_obj_add_event_cb(row, row_edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		text = octopus_group(row, LV_FLEX_FLOW_COLUMN, octopus_px(2));
		lv_obj_set_flex_align(text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
				      LV_FLEX_ALIGN_START);
		lv_obj_align(text, LV_ALIGN_LEFT_MID, octopus_px(12), 0);
		t = label(text, OCTOPUS_FONT_LARGE, a->enabled ? OCTOPUS_COLOR_TEXT :
			  OCTOPUS_COLOR_TEXT_DIM);
		lv_label_set_text_fmt(t, "%02d:%02d", a->time / 60, a->time % 60);
		d = label(text, OCTOPUS_FONT_SMALL, OCTOPUS_COLOR_TEXT_DIM);
		lv_label_set_text(d, octopus_alarm_days_text(a->days, buf, sizeof(buf)));
		octopus_center_y_on(text, row);

		sw = button(row, a->enabled ? "On" : "Off", octopus_font(OCTOPUS_FONT_BODY),
			    octopus_px(72), BAR_H, row_toggle_cb, (void *)(intptr_t)i);
		lv_obj_set_style_bg_color(sw, octopus_color(a->enabled ? OCTOPUS_COLOR_ACCENT :
					  OCTOPUS_COLOR_BG), 0);
		lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -octopus_px(7), 0);
	}
	if (!n_alarms) {
		lv_obj_t *l = label(list, OCTOPUS_FONT_BODY, OCTOPUS_COLOR_TEXT_DIM);

		lv_label_set_text(l, "No alarms.\nTap " LV_SYMBOL_PLUS " to add one.");
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(l, LV_PCT(100));
		lv_obj_set_style_pad_top(l, octopus_px(40), 0);
	}
	if (n_alarms < OCTOPUS_MAX_ALARMS)
		lv_obj_set_hidden(btn_add, false);
	else
		lv_obj_set_hidden(btn_add, true);
}

static void list_show(void)
{
	alarms_load();		/* the shell may have switched a "once" off */
	list_rebuild();
	view_show(view_list);
}

static void build_list(void)
{
	lv_obj_t *b, *title;

	b = button(view_list, LV_SYMBOL_LEFT, octopus_symbol_font(28), TOP_W, BAR_H,
		   list_back_cb, NULL);
	lv_obj_set_pos(b, GAP, TOP_Y);
	title = label(view_list, OCTOPUS_FONT_LARGE, OCTOPUS_COLOR_TEXT);
	lv_label_set_text(title, "Alarms");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
	octopus_center_y_on(title, b);
	btn_add = button(view_list, LV_SYMBOL_PLUS, octopus_symbol_font(28), TOP_W, BAR_H,
			 list_add_cb, NULL);
	lv_obj_align(btn_add, LV_ALIGN_TOP_RIGHT, -GAP, TOP_Y);

	list = lv_obj_create(view_list);
	lv_obj_remove_style_all(list);
	lv_obj_set_size(list, LV_PCT(100), OCTOPUS_APP_H - octopus_px(52));
	lv_obj_set_pos(list, 0, octopus_px(52));
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(list, GAP, 0);
	lv_obj_set_style_pad_bottom(list, GAP, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
}

/* ------------------------------------------------------------------ */
/* Ringing                                                            */

static lv_obj_t *lbl_ring_time;

static void ring_cmd_cb(lv_event_t *e)
{
	octopus_shell_command(lv_event_get_user_data(e));
}

static void build_ring(void)
{
	lv_obj_t *head, *ic, *t, *b, *hint;

	/* Opaque, over all views, swallowing touches meant for them. */
	lv_obj_set_style_bg_opa(view_ring, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(view_ring, octopus_color(OCTOPUS_COLOR_BG), 0);
	lv_obj_set_clickable(view_ring, true);

	head = octopus_group(view_ring, LV_FLEX_FLOW_ROW, octopus_px(10));
	lv_obj_align(head, LV_ALIGN_TOP_MID, 0, octopus_px(12));
	ic = lv_label_create(head);
	lv_label_set_text(ic, LV_SYMBOL_BELL);
	lv_obj_set_style_text_font(ic, octopus_symbol_font(28), 0);
	lv_obj_set_style_text_color(ic, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
	t = label(head, OCTOPUS_FONT_LARGE, OCTOPUS_COLOR_ACCENT);
	lv_label_set_text(t, "Alarm");

	lbl_ring_time = label(view_ring, OCTOPUS_FONT_HUGE, OCTOPUS_COLOR_TEXT);
	lv_obj_align(lbl_ring_time, LV_ALIGN_TOP_MID, 0, 0);

	b = button(view_ring, "Snooze", octopus_font(OCTOPUS_FONT_LARGE),
		   (OCTOPUS_APP_W - octopus_px(3 * 12)) / 2, octopus_px(72), ring_cmd_cb, "snooze");
	lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, octopus_px(12), -octopus_px(28));
	b = button(view_ring, "Stop", octopus_font(OCTOPUS_FONT_LARGE),
		   (OCTOPUS_APP_W - octopus_px(3 * 12)) / 2, octopus_px(72), ring_cmd_cb, "stop");
	lv_obj_set_style_bg_color(b, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
	lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -octopus_px(12), -octopus_px(28));
	octopus_center_y_between(lbl_ring_time, head, b);

	hint = label(view_ring, OCTOPUS_FONT_SMALL, OCTOPUS_COLOR_TEXT_DIM);
	lv_label_set_text(hint, "The top button snoozes too");
	lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 0);
	octopus_center_y_at(hint, OCTOPUS_APP_H - octopus_px(28) / 2);
}

/* ------------------------------------------------------------------ */

/* The time zone changed (Settings): the time again, at the next tick. */
static void zone_changed(void *user)
{
	(void)user;
	shown_minute = -1;
}

/* Twice a second: the shell's alarm state; the time once a minute. */
static void tick(lv_timer_t *t)
{
	time_t now = time(NULL), until = 0;
	octopus_alarm_state_t st = octopus_alarm_state(&until);
	long minute = (long)(now / 60);
	bool changed = st != alarm_state || until != snooze_until;
	/* Synced (or not) since: the warning goes (or comes) right away. */
	bool sync_changed = lv_obj_is_hidden(ic_offline) != octopus_time_synced();

	(void)t;
	if (changed) {
		alarm_state = st;
		snooze_until = until;
		if (st == OCTOPUS_ALARM_RINGING)
			lv_obj_set_hidden(view_ring, false);
		else
			lv_obj_set_hidden(view_ring, true);
	}
	if (changed || sync_changed || minute != shown_minute) {
		char buf[16];
		struct tm tm;

		shown_minute = minute;
		alarms_load();
		clock_update();
		localtime_r(&now, &tm);
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		lv_label_set_text(lbl_ring_time, buf);
	}
}

/* Colors are set per object: rebuild all views in the new palette. */
static void on_mode(octopus_mode_t mode, void *user)
{
	bool list_visible = view_list && !lv_obj_is_hidden(view_list);
	bool edit_visible = view_edit && !lv_obj_is_hidden(view_edit);
	struct octopus_alarm saved = edit;
	int saved_index = edit_index;

	(void)mode;
	(void)user;
	lv_obj_clean(scr);
	view_clock = view_create();
	view_list = view_create();
	view_edit = view_create();
	view_ring = view_create();	/* last: on top */
	build_clock();
	build_list();
	build_edit();
	build_ring();

	if (edit_visible) {
		edit_show(saved_index);
		edit = saved;
		edit_update();
	} else if (list_visible) {
		list_show();
	} else {
		view_show(view_clock);
	}
	alarm_state = -1;	/* re-apply the ringing overlay */
	shown_minute = -1;
	tick(NULL);
}

int main(void)
{
	scr = octopus_app_init("Clock");
	lv_obj_set_scrollable(scr, false);

	alarms_load();
	octopus_on_mode_change(on_mode, NULL);	/* builds the views */
	lv_timer_create(tick, 500, NULL);
	octopus_on_zone_change(zone_changed, NULL);

	return octopus_run();
}
