// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * One Song: the SDK's sample app (docs/sdk-tutorial.md walks through it).
 *
 * One button plays a song online, or stops it. The playing is done by the
 * the system's stream player (octopus-stream, through liboctopus's
 * octopus_stream_*), in a process of its own: the song goes on when this
 * app is in the background, frozen or closed. So the app never keeps state
 * of its own about playing: twice a second it asks the player what plays,
 * and shows that.
 */

#include <stdio.h>
#include <string.h>
#include <octopus.h>

#define APP_ID	"OneSong"	/* = Id in app.conf */
#define SONG	"https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3"
#define TITLE	"SoundHelix Song 1"

static lv_obj_t *button, *button_icon, *title, *status;

/* Is our song the one playing (or connecting)? */
static bool ours(struct octopus_stream_status *st)
{
	return octopus_stream_status(st) && !strcmp(st->url, SONG);
}

static void pressed(lv_event_t *e)
{
	struct octopus_stream_status st;

	(void)e;
	if (ours(&st))
		octopus_stream_stop();
	else
		octopus_stream_play(SONG, TITLE);	/* stops FM, AirPlay, another stream */
	lv_label_set_text(status, "...");	/* until the player says */
}

/* What the player says, twice a second. */
static void poll(lv_timer_t *t)
{
	struct octopus_stream_status st;
	char text[64];

	(void)t;
	if (!ours(&st)) {
		lv_label_set_text(button_icon, LV_SYMBOL_PLAY);
		lv_label_set_text(status, "Tap to play");
		return;
	}
	lv_label_set_text(button_icon, LV_SYMBOL_STOP);
	if (!strcmp(st.state, "playing"))
		snprintf(text, sizeof(text), "Playing  %d:%02d", st.played_s / 60, st.played_s % 60);
	else if (!strcmp(st.state, "error"))
		snprintf(text, sizeof(text), "Can't play it (no network?)");
	else
		snprintf(text, sizeof(text), "Connecting...");
	lv_label_set_text(status, text);
}

/* Colours are per mode (day, night): set here, called now and at each
 * switch. */
static void on_mode(octopus_mode_t mode, void *user)
{
	(void)mode; (void)user;
	octopus_glossy(button, octopus_color(OCTOPUS_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(button_icon, octopus_on_accent(), 0);
	lv_obj_set_style_text_color(title, octopus_color(OCTOPUS_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(status, octopus_color(OCTOPUS_COLOR_TEXT_DIM), 0);
}

int main(void)
{
	lv_obj_t *scr = octopus_app_init(APP_ID);

	lv_obj_set_scrollable(scr, false);

	/* The button: big and round (a finger on a resistive screen). */
	button = lv_button_create(scr);
	lv_obj_set_size(button, octopus_px(96), octopus_px(96));
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_align(button, LV_ALIGN_CENTER, 0, -octopus_px(24));
	lv_obj_add_event_cb(button, pressed, LV_EVENT_CLICKED, NULL);
	button_icon = lv_label_create(button);
	lv_obj_set_style_text_font(button_icon, octopus_symbol_font(48), 0);
	lv_obj_center(button_icon);

	title = lv_label_create(scr);
	lv_obj_set_style_text_font(title, octopus_font(OCTOPUS_FONT_LARGE), 0);
	lv_label_set_text(title, TITLE);
	lv_obj_align_to(title, button, LV_ALIGN_OUT_BOTTOM_MID, 0, octopus_px(14));

	status = lv_label_create(scr);
	lv_obj_set_style_text_font(status, octopus_font(OCTOPUS_FONT_SMALL), 0);
	lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(status, OCTOPUS_APP_W);	/* centred however long the text */
	lv_obj_align_to(status, title, LV_ALIGN_OUT_BOTTOM_MID, 0, octopus_px(6));

	octopus_on_mode_change(on_mode, NULL);
	lv_timer_ready(lv_timer_create(poll, 500, NULL));
	return octopus_run();
}
