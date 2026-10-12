// A new Octopus app (octopus-app new): a label in the middle of its window.
#include <octopus.h>

static lv_obj_t *label;

/* Colours are per mode (day, night): set them here - called once now and
 * at every switch - not once at start. */
static void on_mode(octopus_mode_t mode, void *user)
{
	(void)mode; (void)user;
	lv_obj_set_style_text_color(label, octopus_color(OCTOPUS_COLOR_TEXT), 0);
}

int main(void)
{
	lv_obj_t *scr = octopus_app_init("Hello");	/* = Id in app.conf */

	lv_obj_set_scrollable(scr, false);
	label = lv_label_create(scr);
	lv_obj_set_style_text_font(label, octopus_font(OCTOPUS_FONT_LARGE), 0);
	lv_label_set_text(label, "Hello, Octopus");
	lv_obj_center(label);
	octopus_on_mode_change(on_mode, NULL);
	return octopus_run();
}
