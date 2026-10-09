// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-calibrate: touchscreen calibration.
 *
 * Five targets, one at a time (four corners, then the centre); each touch
 * is read raw from the touchscreen (evdev), its first and last samples
 * dropped (the resistive panel settles at both ends of a press) and the
 * rest's median kept. A least-squares fit gives libinput's calibration
 * matrix (normalised device to normalised screen coordinates), saved to
 * /var/lib/chumby/touch.cal; chumby-wm applies it as soon as it changes.
 * A fit that misses a target by too much starts over.
 *
 * Then a try-out screen: a dot follows the finger, and Again / Done.
 * Everything here works from the raw readings and the new fit, not
 * through the compositor's calibration, so a wildly wrong old one can't
 * lock you out.
 *
 * The shell runs this first while there is no calibration, and when the
 * snooze button is held 4 s (no menu entry: that works however far off
 * touch is). chumby-wm shows it over the whole screen.
 */

#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <chumby.h>

#define TOUCH_NAME	"mxs-lradc-ts"
#define CAL_FILE	"/var/lib/chumby/touch.cal"
#define N_TARGETS	5
#define INSET		24	/* corner targets from the edges */
#define MAX_ERR_PX	12	/* worst miss a fit may have */
#define MAX_SAMPLES	256
#define W		CHUMBY_SCREEN_W
#define H		CHUMBY_SCREEN_H

static const int targets[N_TARGETS][2] = {
	{ INSET, INSET }, { W - INSET, INSET }, { W - INSET, H - INSET },
	{ INSET, H - INSET }, { W / 2, H / 2 },
};

static int fd = -1;
static int x_min, x_max, y_min, y_max;
static int raw_x, raw_y;
static bool touching;
static int sx[MAX_SAMPLES], sy[MAX_SAMPLES], n_samples;

static double got[N_TARGETS][2];	/* normalised readings per target */
static int step;			/* target being shown; N_TARGETS = try-out */
static double m[6];			/* the fit */

static lv_obj_t *scr, *target, *lbl_prompt, *lbl_step, *dot, *btn_again, *btn_done;
static lv_timer_t *restart_timer;

static int find_touch(void)
{
	char dev[32], name[64];
	int i, f;

	for (i = 0; i < 16; i++) {
		snprintf(dev, sizeof(dev), "/dev/input/event%d", i);
		f = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (f < 0)
			continue;
		if (ioctl(f, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, TOUCH_NAME))
			return f;
		close(f);
	}
	return -1;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/* ------------------------------------------------------------------ */
/* The fit                                                            */

/* Solve the 3x3 system a p = b (Cramer's rule). */
static bool solve3(double a[3][3], const double b[3], double p[3])
{
	double d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
		   a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
		   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	double t[3][3];
	int i, j, k;

	if (fabs(d) < 1e-12)
		return false;
	for (k = 0; k < 3; k++) {
		for (i = 0; i < 3; i++)
			for (j = 0; j < 3; j++)
				t[i][j] = j == k ? b[i] : a[i][j];
		p[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) -
			t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
			t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / d;
	}
	return true;
}

/* Least squares: screen u = a x + b y + c (and v likewise), all
 * normalised to 0..1. */
static bool fit(void)
{
	double ata[3][3] = { { 0 } }, atu[3] = { 0 }, atv[3] = { 0 };
	int i, j, k;

	for (i = 0; i < N_TARGETS; i++) {
		double row[3] = { got[i][0], got[i][1], 1 };
		double u = (double)targets[i][0] / W, v = (double)targets[i][1] / H;

		for (j = 0; j < 3; j++) {
			for (k = 0; k < 3; k++)
				ata[j][k] += row[j] * row[k];
			atu[j] += row[j] * u;
			atv[j] += row[j] * v;
		}
	}
	return solve3(ata, atu, m) && solve3(ata, atv, m + 3);
}

static void map(double x, double y, int *px, int *py)
{
	*px = (int)lround((m[0] * x + m[1] * y + m[2]) * W);
	*py = (int)lround((m[3] * x + m[4] * y + m[5]) * H);
}

static double worst_miss(void)
{
	double worst = 0, d;
	int i, px, py;

	for (i = 0; i < N_TARGETS; i++) {
		map(got[i][0], got[i][1], &px, &py);
		d = hypot(px - targets[i][0], py - targets[i][1]);
		if (d > worst)
			worst = d;
	}
	return worst;
}

static bool save(void)
{
	char tmp[] = CAL_FILE ".tmp";
	FILE *f = fopen(tmp, "w");

	if (!f)
		return false;
	fprintf(f, "%.5f %.5f %.5f %.5f %.5f %.5f\n", m[0], m[1], m[2], m[3], m[4], m[5]);
	if (fclose(f))
		return false;
	return rename(tmp, CAL_FILE) == 0;	/* chumby-wm watches for this */
}

/* ------------------------------------------------------------------ */
/* Screens                                                            */

static void show_target(void)
{
	lv_obj_set_hidden(dot, true);
	lv_obj_set_hidden(btn_again, true);
	lv_obj_set_hidden(btn_done, true);
	lv_obj_set_hidden(target, false);
	lv_obj_set_pos(target, targets[step][0] - lv_obj_get_width(target) / 2,
		       targets[step][1] - lv_obj_get_height(target) / 2);
	lv_label_set_text(lbl_prompt, "Touch the centre of the target");
	lv_label_set_text_fmt(lbl_step, "%d of %d", step + 1, N_TARGETS);
}

static void start_over(const char *why)
{
	step = 0;
	show_target();
	if (why)
		lv_label_set_text(lbl_prompt, why);
}

static void restart_cb(lv_timer_t *t)
{
	(void)t;
	lv_timer_pause(restart_timer);
	start_over(NULL);
}

static void show_tryout(void)
{
	lv_obj_set_hidden(target, true);
	lv_obj_set_hidden(btn_again, false);
	lv_obj_set_hidden(btn_done, false);
	lv_label_set_text(lbl_prompt, "Calibrated. Touch anywhere to try it.");
	lv_label_set_text(lbl_step, "");
}

static bool in_button(lv_obj_t *b, int px, int py)
{
	lv_area_t a;

	lv_obj_get_coords(b, &a);
	/* Generous: a resistive tap lands a few pixels off. */
	return px >= a.x1 - 12 && px <= a.x2 + 12 && py >= a.y1 - 12 && py <= a.y2 + 12;
}

/* A finished press, normalised. */
static void pressed(double x, double y)
{
	int px, py;

	if (step < N_TARGETS) {
		got[step][0] = x;
		got[step][1] = y;
		if (++step < N_TARGETS) {
			show_target();
			return;
		}
		if (!fit() || worst_miss() > MAX_ERR_PX) {
			lv_obj_set_hidden(target, true);
			lv_label_set_text(lbl_prompt, "That didn't line up. Once more...");
			lv_label_set_text(lbl_step, "");
			lv_timer_reset(restart_timer);
			lv_timer_resume(restart_timer);
			return;
		}
		if (!save()) {
			start_over("Couldn't save it. Once more...");
			return;
		}
		show_tryout();
		return;
	}
	/* Try-out: the buttons, by the new fit. */
	map(x, y, &px, &py);
	if (in_button(btn_done, px, py))
		exit(0);
	if (in_button(btn_again, px, py))
		start_over(NULL);
}

/* While a finger is down on the try-out screen: the dot follows it. */
static void moved(double x, double y)
{
	int px, py;

	if (step < N_TARGETS)
		return;
	map(x, y, &px, &py);
	lv_obj_set_hidden(dot, false);
	lv_obj_set_pos(dot, px - lv_obj_get_width(dot) / 2, py - lv_obj_get_height(dot) / 2);
}

static double norm(int v, int lo, int hi)
{
	return hi > lo ? (double)(v - lo) / (hi - lo) : 0;
}

static void touch_readable(int f, void *user)
{
	struct input_event ev;
	int keep, from, x, y;

	(void)user;
	while (read(f, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_ABS && ev.code == ABS_X)
			raw_x = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_Y)
			raw_y = ev.value;
		else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value) {
			touching = true;
			n_samples = 0;
		} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && !ev.value && touching) {
			touching = false;
			if (!n_samples)		/* a tap too short to sample */
				continue;
			/* Drop the settling samples at both ends, keep the median. */
			from = n_samples > 6 ? 2 : 0;
			keep = n_samples - 2 * from;
			qsort(sx + from, keep, sizeof(int), cmp_int);
			qsort(sy + from, keep, sizeof(int), cmp_int);
			x = sx[from + keep / 2];
			y = sy[from + keep / 2];
			pressed(norm(x, x_min, x_max), norm(y, y_min, y_max));
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT && touching) {
			if (n_samples < MAX_SAMPLES) {
				sx[n_samples] = raw_x;
				sy[n_samples] = raw_y;
				n_samples++;
			}
			moved(norm(raw_x, x_min, x_max), norm(raw_y, y_min, y_max));
		}
	}
}

/* ------------------------------------------------------------------ */

static lv_obj_t *line(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(o, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_clickable(o, false);
	return o;
}

static lv_obj_t *button(const char *text, int x)
{
	lv_obj_t *b = lv_button_create(scr), *l = lv_label_create(b);

	lv_obj_set_size(b, 120, 44);
	lv_obj_set_pos(b, x, H - 44 - 16);
	lv_obj_set_style_radius(b, 10, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_clickable(b, false);		/* hit-tested from raw input */
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_center(l);
	return b;
}

int main(void)
{
	struct input_absinfo ax, ay;
	lv_display_t *disp;
	lv_obj_t *ring;

	fd = find_touch();
	if (fd < 0 || ioctl(fd, EVIOCGABS(ABS_X), &ax) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &ay) < 0) {
		fprintf(stderr, "chumby-calibrate: no %s touchscreen\n", TOUCH_NAME);
		return 1;
	}
	x_min = ax.minimum;
	x_max = ax.maximum;
	y_min = ay.minimum;
	y_max = ay.maximum;

	chumby_init();
	disp = chumby_window_create("chumby-calibrate", W, H);
	if (!disp)
		return 1;
	scr = lv_display_get_screen_active(disp);
	lv_obj_set_scrollable(scr, false);

	/* The target: a crosshair with a ring. */
	target = lv_obj_create(scr);
	lv_obj_remove_style_all(target);
	lv_obj_set_size(target, 33, 33);
	lv_obj_set_clickable(target, false);
	line(target, 0, 15, 33, 3);
	line(target, 15, 0, 3, 33);
	ring = lv_obj_create(target);
	lv_obj_remove_style_all(ring);
	lv_obj_set_size(ring, 17, 17);
	lv_obj_center(ring);
	lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(ring, 3, 0);
	lv_obj_set_style_border_color(ring, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_clickable(ring, false);

	lbl_prompt = lv_label_create(scr);
	lv_obj_set_width(lbl_prompt, W - 2 * 56);
	lv_obj_set_style_text_align(lbl_prompt, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lbl_prompt, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_label_set_long_mode(lbl_prompt, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_align(lbl_prompt, LV_ALIGN_TOP_MID, 0, 62);
	lbl_step = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_step, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_align(lbl_step, LV_ALIGN_TOP_MID, 0, 156);

	dot = lv_obj_create(scr);
	lv_obj_remove_style_all(dot);
	lv_obj_set_size(dot, 14, 14);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(dot, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_clickable(dot, false);

	btn_again = button("Again", 24);
	btn_done = button("Done", W - 24 - 120);
	lv_obj_set_style_bg_color(btn_done, chumby_color(CHUMBY_COLOR_ACCENT), 0);

	restart_timer = lv_timer_create(restart_cb, 1500, NULL);
	lv_timer_pause(restart_timer);
	start_over(NULL);
	chumby_watch_fd(fd, touch_readable, NULL);
	return chumby_run();
}
