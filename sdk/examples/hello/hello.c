// A new Chumby app (chumby-app new): a label in the middle of its window.
#include <chumby.h>

static lv_obj_t *label;

/* Colours are per mode (day, night): set them here - called once now and
 * at every switch - not once at start. */
static void on_mode(chumby_mode_t mode, void *user)
{
	(void)mode; (void)user;
	lv_obj_set_style_text_color(label, chumby_color(CHUMBY_COLOR_TEXT), 0);
}

int main(void)
{
	lv_obj_t *scr = chumby_app_init("Hello");	/* = Id in app.conf */

	lv_obj_set_scrollable(scr, false);
	label = lv_label_create(scr);
	lv_obj_set_style_text_font(label, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_label_set_text(label, "Hello, Chumby");
	lv_obj_center(label);
	chumby_on_mode_change(on_mode, NULL);
	return chumby_run();
}
