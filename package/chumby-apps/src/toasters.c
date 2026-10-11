// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-toasters: Flying Toasters, the After Dark screen saver.
 *
 * The shell starts us after a while without a touch (chumby-wm tells it),
 * unless the clock is in front; a tap (anything, really: the shell closes
 * us at any input) ends it. We only hold the screen: our window
 * ("chumby-saver") covers it, bar too, and chumby-wm flies the toasters
 * over it while it's up (package/chumby-wm/src/toasters.c: scene nodes it
 * moves, no redraws here). As an LVGL app drawing them itself the saver
 * cost the Chumby's ARM9 ~40%: LVGL's bookkeeping per object and redrawn
 * area, its 16-bit window converted for the 32-bit screen, two processes'
 * frames.
 *
 * With a GPU the window is see-through: chumby-wm blurs and dims the screen
 * under it. Without (the Chumby: pixman) it's black, as After Dark was: so
 * the compositor skips what's under it.
 */

#include <stdlib.h>
#include <unistd.h>
#include <chumby.h>

#define RENDER_NODE	"/dev/dri/renderD128"	/* a GPU: see-through */

static void tapped(lv_event_t *e)
{
	(void)e;
	exit(0);
}

int main(void)
{
	bool see_through = access(RENDER_NODE, F_OK) == 0;
	lv_display_t *disp;
	lv_obj_t *scr;

	chumby_init();
	if (see_through)
		lv_wayland_window_set_next_color_format(LV_COLOR_FORMAT_ARGB8888);
	disp = chumby_window_create("chumby-saver", CHUMBY_SCREEN_W, CHUMBY_SCREEN_H);
	if (!disp)
		return 1;
	scr = lv_display_get_screen_active(disp);
	lv_obj_clean(scr);
	lv_obj_set_scrollable(scr, false);
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, see_through ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
	lv_obj_add_event_cb(scr, tapped, LV_EVENT_PRESSED, NULL);
	return chumby_run();
}
