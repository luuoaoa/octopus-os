// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-airplay: what an iPhone, iPad or Mac plays to us over AirPlay,
 * laid out like Internet Radio:
 *
 *   left        the cover art
 *   right       "AirPlay" and the sending device, small, level with the
 *               art's top; under it the title (scrolling twice when it
 *               changes, then cut with dots), the artist and album
 *   bottom      stop (ends the AirPlay session: the phone stops sending)
 *
 * shairport-sync is the receiver (an AirPlay 1 speaker named after the
 * host). When something starts playing, its hook makes the other sources
 * stop (chumby-audio play airplay) and brings this app up through the
 * shell; chumby-airplay-meta keeps what plays in $run_dir/airplay, which
 * we read twice a second. Volume is the phone's (shairport-sync turns the
 * stream down) and the shell's, as for everything.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chumby.h>

#define AUDIO		"/usr/bin/chumby-audio"
#define ART_X		14
#define ART_SIZE	92
#define COL_X		(ART_X + ART_SIZE + 12)
#define COL_W		(CHUMBY_APP_W - 10 - COL_X)
#define LINE_GAP	2

static char st_state[16], st_title[256], st_artist[256], st_album[256], st_client[128];
static char st_cover[96];
static unsigned st_cover_id = ~0u, shown_cover_id = ~0u;
static int audio_child = -1;

static lv_obj_t *scr, *lbl_top, *lbl_title, *lbl_sub, *art_tile, *art_img, *art_note;
static lv_obj_t *btn_stop, *lbl_hint;

static void state_read(void)
{
	char path[256], line[400];
	FILE *f;

	st_state[0] = st_title[0] = st_artist[0] = st_album[0] = st_client[0] = st_cover[0] = '\0';
	snprintf(path, sizeof(path), "%s/airplay", chumby_run_dir());
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "state=", 6))
			snprintf(st_state, sizeof(st_state), "%.15s", line + 6);
		else if (!strncmp(line, "title=", 6))
			snprintf(st_title, sizeof(st_title), "%.255s", line + 6);
		else if (!strncmp(line, "artist=", 7))
			snprintf(st_artist, sizeof(st_artist), "%.255s", line + 7);
		else if (!strncmp(line, "album=", 6))
			snprintf(st_album, sizeof(st_album), "%.255s", line + 6);
		else if (!strncmp(line, "client=", 7))
			snprintf(st_client, sizeof(st_client), "%.127s", line + 7);
		else if (!strncmp(line, "cover=", 6))
			snprintf(st_cover, sizeof(st_cover), "%.95s", line + 6);
		else if (!strncmp(line, "cover_id=", 9))
			st_cover_id = (unsigned)strtoul(line + 9, NULL, 10);
	}
	if (f)
		fclose(f);
	if (!st_state[0])
		snprintf(st_state, sizeof(st_state), "stopped");
}

static bool active(void)
{
	return strcmp(st_state, "stopped") != 0;
}

static void art_show_note(void)
{
	lv_obj_set_hidden(art_img, true);
	lv_obj_set_hidden(art_note, false);
	lv_obj_set_style_bg_color(art_tile, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(art_note, chumby_on_accent(), 0);
	lv_obj_set_style_bg_grad_color(art_tile, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_VER, 0);
}

static void art_update(void)
{
	if (st_cover_id == shown_cover_id && (st_cover[0] != 0) == !lv_obj_is_hidden(art_img))
		return;
	shown_cover_id = st_cover_id;
	if (!active() || !st_cover[0] || !chumby_logo_set(art_img, st_cover, ART_SIZE)) {
		art_show_note();
		return;
	}
	lv_obj_center(art_img);
	lv_obj_set_hidden(art_img, false);
	lv_obj_set_hidden(art_note, true);
	lv_obj_set_style_bg_color(art_tile, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_NONE, 0);
}

static void ui_update(void)
{
	char top[160], sub[520], host[64] = "this Chumby";

	if (active()) {
		if (st_client[0])
			snprintf(top, sizeof(top), "AirPlay from %s", st_client);
		else
			snprintf(top, sizeof(top), "AirPlay");
		if (st_artist[0] && st_album[0])
			snprintf(sub, sizeof(sub), "%s - %s", st_artist, st_album);
		else
			snprintf(sub, sizeof(sub), "%s%s", st_artist, st_album);
		if (!strcmp(st_state, "paused"))
			snprintf(sub, sizeof(sub), "paused");
		chumby_scroll_label_set(lbl_title, st_title[0] ? st_title : "Playing");
	} else {
		snprintf(top, sizeof(top), "AirPlay");
		gethostname(host, sizeof(host) - 1);
		snprintf(sub, sizeof(sub), "Ready");
		chumby_scroll_label_set(lbl_title, "Nothing playing");
		lv_label_set_text_fmt(lbl_hint, "On an iPhone, iPad or Mac, choose \"%s\" as the speaker.",
				      host);
	}
	lv_label_set_text(lbl_top, top);
	if (strcmp(lv_label_get_text(lbl_sub), sub))
		lv_label_set_text(lbl_sub, sub);
	lv_obj_set_hidden(btn_stop, !active());
	lv_obj_set_hidden(lbl_hint, active());
	art_update();
}

static void poll_cb(lv_timer_t *t)
{
	int status;

	(void)t;
	if (audio_child > 0 && waitpid(audio_child, &status, WNOHANG) == audio_child)
		audio_child = -1;
	state_read();
	ui_update();
}

/* Ends the AirPlay session (the phone stops sending), like the volume
 * control's stop. */
static void stop_cb(lv_event_t *e)
{
	char *argv[] = { "chumby-audio", "stop", NULL };

	(void)e;
	if (audio_child <= 0)
		audio_child = chumby_spawn(AUDIO, argv);
}

static void on_mode(chumby_mode_t mode, void *user)
{
	(void)mode;
	(void)user;
	lv_obj_set_style_text_color(lbl_top, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_title, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_sub, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_hint, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_bg_color(btn_stop, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	if (!lv_obj_is_hidden(art_note))
		art_show_note();
}

int main(void)
{
	int32_t art_y, title_y, line_large, line_body;
	lv_obj_t *l;

	scr = chumby_app_init("AirPlay");
	lv_obj_set_scrollable(scr, false);
	line_large = lv_font_get_line_height(chumby_font(CHUMBY_FONT_LARGE));
	line_body = lv_font_get_line_height(chumby_font(CHUMBY_FONT_BODY));

	/* Bottom: stop, while something plays; else how to start. */
	btn_stop = lv_button_create(scr);
	lv_obj_set_size(btn_stop, 56, 56);
	lv_obj_set_style_radius(btn_stop, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn_stop, 0, 0);
	lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_MID, 0, -8);
	l = lv_label_create(btn_stop);
	lv_label_set_text(l, LV_SYMBOL_STOP);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_obj_center(l);
	lv_obj_add_event_cb(btn_stop, stop_cb, LV_EVENT_CLICKED, NULL);
	lbl_hint = lv_label_create(scr);
	lv_obj_set_width(lbl_hint, CHUMBY_APP_W - 2 * ART_X);
	lv_label_set_long_mode(lbl_hint, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(lbl_hint, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_style_text_align(lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(lbl_hint, "");
	lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -14);
	lv_obj_update_layout(scr);

	/* The art, centred in the space above the buttons (as the radios). */
	art_y = (lv_obj_get_y(btn_stop) - ART_SIZE) / 2;
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
	lv_label_set_text(art_note, CHUMBY_ICON_AIRPLAY);
	lv_obj_set_style_text_font(art_note, chumby_icon_font(28), 0);
	lv_obj_set_style_transform_scale(art_note, 384, 0);	/* 1.5x */
	lv_obj_center(art_note);

	lbl_top = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_top, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_width(lbl_top, COL_W);
	lv_obj_set_height(lbl_top, line_body);
	lv_label_set_long_mode(lbl_top, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(lbl_top, COL_X, art_y);
	lv_label_set_text(lbl_top, "");

	title_y = art_y + line_body + LINE_GAP;
	lbl_title = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_title, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(lbl_title, COL_W);
	chumby_scroll_label_init(lbl_title);
	lv_obj_set_pos(lbl_title, COL_X, title_y);
	lbl_sub = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_sub, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_width(lbl_sub, COL_W);
	lv_obj_set_height(lbl_sub, line_body);
	lv_label_set_long_mode(lbl_sub, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(lbl_sub, "");
	lv_obj_set_pos(lbl_sub, COL_X, title_y + line_large + LINE_GAP);

	chumby_on_mode_change(on_mode, NULL);
	state_read();
	ui_update();
	lv_timer_create(poll_cb, 500, NULL);

	return chumby_run();
}
