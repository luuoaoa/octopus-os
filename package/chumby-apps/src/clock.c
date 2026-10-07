// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-clock: full-window clock. Time, seconds and date; restyled by the
 * framework's day/night policy.
 */

#include <time.h>
#include <chumby.h>

static lv_obj_t *lbl_time, *lbl_sec, *lbl_date, *lbl_mode;

static void tick(lv_timer_t *t)
{
	char buf[48];
	time_t now = time(NULL);
	struct tm tm;

	(void)t;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), "%H:%M", &tm);
	lv_label_set_text(lbl_time, buf);
	strftime(buf, sizeof(buf), ":%S", &tm);
	lv_label_set_text(lbl_sec, buf);
	strftime(buf, sizeof(buf), "%A, %d %B", &tm);
	lv_label_set_text(lbl_date, buf);
}

static void on_mode(chumby_mode_t mode, void *user)
{
	(void)user;
	lv_obj_set_style_text_color(lbl_time, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_sec, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(lbl_date, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_mode, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_label_set_text(lbl_mode, mode == CHUMBY_MODE_NIGHT ? LV_SYMBOL_EYE_CLOSE "  night" : "");
}

int main(void)
{
	lv_obj_t *scr = chumby_app_init("Clock");

	lv_obj_set_scrollable(scr, false);

	lbl_time = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_time, chumby_font(CHUMBY_FONT_HUGE), 0);
	lv_obj_align(lbl_time, LV_ALIGN_CENTER, -18, -24);

	lbl_sec = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_sec, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_align_to(lbl_sec, lbl_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -10);

	lbl_date = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_date, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, 30);

	lbl_mode = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_mode, chumby_font(CHUMBY_FONT_SMALL), 0);
	lv_obj_align(lbl_mode, LV_ALIGN_BOTTOM_MID, 0, -8);

	tick(NULL);
	/* The seconds label moves with the width of HH:MM. */
	lv_obj_update_layout(scr);
	lv_obj_align_to(lbl_sec, lbl_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -10);
	chumby_on_mode_change(on_mode, NULL);
	lv_timer_create(tick, 1000, NULL);

	return chumby_run();
}
