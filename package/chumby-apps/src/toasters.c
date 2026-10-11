// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-toasters: Flying Toasters, the After Dark screen saver, as After
 * Dark CSS draws it (https://www.bryanbraun.com/after-dark-css/): toasters
 * flapping their wings and slices of toast gliding down from the top right
 * to the bottom left, at three speeds, over and over.
 *
 * The shell starts us after a while without a touch (chumby-wm tells it),
 * unless the clock is in front; a tap (anything, really: the shell closes
 * us at any input) ends it. Our window ("chumby-saver") covers the whole
 * screen, bar too, and is see-through: chumby-wm puts the screen under it
 * blurred and dimmed (on a GPU), or dimmed (pixman).
 *
 * The pictures are After Dark CSS's GIFs (toaster-sprite.gif: four 64 px
 * wing frames side by side; toast1.gif), (c) Berkeley Systems: fetched
 * when the image is built (package/after-dark-toasters), decoded here
 * (a small GIF reader: they're plain, one frame each) and drawn at half
 * size, as big against our 320x240 as against After Dark's 640x480.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <chumby.h>

#define DIR		"/usr/share/chumby/toasters"
#define W		CHUMBY_SCREEN_W
#define H		CHUMBY_SCREEN_H
#define SRC		64		/* the GIFs' frame size */
#define SZ		32		/* ours */
#define FRAMES		4
#define N_TOASTERS	8
#define N_TOAST		4
#define N		(N_TOASTERS + N_TOAST)
#define TICK_MS		40
#define FLAP_STEP_MS	50		/* .2 s for the four frames, then back */
#define GAP		(SZ + 4)	/* apart, at least, when they come in */

/* ------------------------------------------------------------------ */
/* GIF                                                                */

struct gif {
	int w, h;
	uint32_t *argb;		/* w x h, straight alpha */
};

struct bits {
	const uint8_t *p, *end;	/* the image's data sub-blocks, joined */
	uint32_t acc;
	int n;
};

static int bits_get(struct bits *b, int size)
{
	int v;

	while (b->n < size) {
		if (b->p >= b->end)
			return -1;
		b->acc |= (uint32_t)*b->p++ << b->n;
		b->n += 8;
	}
	v = b->acc & ((1u << size) - 1);
	b->acc >>= size;
	b->n -= size;
	return v;
}

/* LZW, as GIF has it: the pixels' colour indices into out[count]. */
static int lzw(const uint8_t *data, size_t len, int min_size, uint8_t *out, size_t count)
{
	static uint16_t prefix[4096];
	static uint8_t suffix[4096], stack[4097];
	struct bits b = { data, data + len, 0, 0 };
	int clear = 1 << min_size, end = clear + 1, next = clear + 2;
	int size = min_size + 1, prev = -1, code, first = 0;
	size_t o = 0;

	for (code = 0; code < clear; code++) {
		prefix[code] = 0;
		suffix[code] = code;
	}
	while (o < count && (code = bits_get(&b, size)) >= 0) {
		int c, sp = 0;

		if (code == clear) {
			size = min_size + 1;
			next = clear + 2;
			prev = -1;
			continue;
		}
		if (code == end)
			break;
		if (prev < 0) {			/* the first after a clear */
			out[o++] = first = suffix[code];
			prev = code;
			continue;
		}
		c = code;
		if (code >= next) {		/* the one being made: prev + its first */
			stack[sp++] = first;
			c = prev;
		}
		while (c >= clear && sp < 4096) {
			stack[sp++] = suffix[c];
			c = prefix[c];
		}
		stack[sp++] = first = suffix[c];
		while (sp && o < count)
			out[o++] = stack[--sp];
		if (next < 4096) {
			prefix[next] = prev;
			suffix[next] = first;
			next++;
			if (next == 1 << size && size < 12)
				size++;
		}
		prev = code;
	}
	return o == count ? 0 : -1;
}

/* The first image of a GIF file, its transparent colour clear. */
static bool gif_load(const char *path, struct gif *g)
{
	uint8_t *f = NULL, *p, *end, *data = NULL, *idx = NULL;
	uint8_t pal[256 * 3] = { 0 };
	int trans = -1, x, y;
	size_t len = 0, dlen = 0;
	FILE *fp = fopen(path, "rb");
	bool ok = false;

	if (!fp)
		return false;
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	f = malloc(len);
	if (!f || fread(f, 1, len, fp) != len || len < 13 || memcmp(f, "GIF8", 4))
		goto out;
	end = f + len;
	g->w = f[6] | f[7] << 8;
	g->h = f[8] | f[9] << 8;
	p = f + 13;
	if (f[10] & 0x80) {		/* the global colour table */
		int n = 2 << (f[10] & 7);

		if (p + n * 3 > end)
			goto out;
		memcpy(pal, p, n * 3);
		p += n * 3;
	}
	while (p < end) {
		if (*p == 0x21) {		/* an extension */
			if (p + 2 > end)
				goto out;
			if (p[1] == 0xf9 && p + 7 < end && (p[3] & 1))
				trans = p[6];	/* graphic control: transparency */
			p += 2;
			while (p < end && *p)
				p += *p + 1;
			p++;
		} else if (*p == 0x2c) {	/* the image */
			int iw, ih, ix, iy, flags, min;
			uint8_t *q;

			if (p + 10 > end)
				goto out;
			ix = p[1] | p[2] << 8;
			iy = p[3] | p[4] << 8;
			iw = p[5] | p[6] << 8;
			ih = p[7] | p[8] << 8;
			flags = p[9];
			p += 10;
			if (flags & 0x80) {	/* a local colour table */
				int n = 2 << (flags & 7);

				if (p + n * 3 > end)
					goto out;
				memcpy(pal, p, n * 3);
				p += n * 3;
			}
			if (p >= end)
				goto out;
			min = *p++;
			data = malloc(end - p);
			if (!data)
				goto out;
			for (q = p; q < end && *q; q += *q + 1) {
				if (q + 1 + *q > end)
					goto out;
				memcpy(data + dlen, q + 1, *q);
				dlen += *q;
			}
			idx = malloc((size_t)iw * ih);
			g->argb = calloc((size_t)g->w * g->h, 4);
			if (!data || !idx || !g->argb || min < 2 || min > 8 ||
			    lzw(data, dlen, min, idx, (size_t)iw * ih) < 0)
				goto out;
			/* (No interlacing: these aren't.) */
			for (y = 0; y < ih; y++)
				for (x = 0; x < iw; x++) {
					int c = idx[y * iw + x], gx = ix + x, gy = iy + y;

					if (c == trans || gx >= g->w || gy >= g->h)
						continue;
					g->argb[gy * g->w + gx] = 0xff000000u |
						pal[c * 3] << 16 | pal[c * 3 + 1] << 8 | pal[c * 3 + 2];
				}
			ok = true;
			break;
		} else {
			break;
		}
	}
out:
	if (!ok) {
		free(g->argb);
		g->argb = NULL;
	}
	free(idx);
	free(data);
	free(f);
	fclose(fp);
	return ok;
}

/* A SRC x SRC square of the GIF, at x0, halved (2x2 averaged, by alpha:
 * the outlines stay) into an LVGL image (a draw buffer: an image's
 * source as it is). */
static lv_draw_buf_t *frame_make(const struct gif *g, int x0)
{
	lv_draw_buf_t *buf = lv_draw_buf_create(SZ, SZ, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
	int x, y, k;

	if (!buf)
		return NULL;
	for (y = 0; y < SZ; y++) {
		uint32_t *row = (uint32_t *)(buf->data + y * buf->header.stride);

		for (x = 0; x < SZ; x++) {
			unsigned a = 0, r = 0, gr = 0, bl = 0;

			for (k = 0; k < 4; k++) {
				int sx = x0 + 2 * x + (k & 1), sy = 2 * y + (k >> 1);
				uint32_t px = sx < g->w && sy < g->h ? g->argb[sy * g->w + sx] : 0;
				unsigned pa = px >> 24;

				a += pa;
				r += (px >> 16 & 0xff) * pa;
				gr += (px >> 8 & 0xff) * pa;
				bl += (px & 0xff) * pa;
			}
			row[x] = a ? (a / 4) << 24 | (r / a) << 16 | (gr / a) << 8 | bl / a : 0;
		}
	}
	return buf;
}

/* ------------------------------------------------------------------ */
/* Flying                                                             */

struct sprite {
	lv_obj_t *img;
	bool toaster;
	float x, y, v;		/* top left; px/s down and left alike */
	int phase;		/* the wings: where in their beat */
	int frame;		/* shown */
};

static lv_draw_buf_t *wings[FRAMES], *toast;
static struct sprite sprites[N];
static uint32_t last_ms;
/* Moved, not drawn yet: wait. Moving again first would give LVGL more
 * areas to redraw than it keeps (32: each sprite's old and new place),
 * and it would redraw the whole screen instead. */
static bool moved;

/* After Dark CSS's three speeds (1600 px in 10, 16, 24 s), at our size. */
static const float speeds[] = { 40.0f, 25.0f, 17.0f };

static bool clear_of_others(const struct sprite *s)
{
	int i;

	for (i = 0; i < N; i++) {
		const struct sprite *o = &sprites[i];

		if (o != s && o->img && abs((int)(o->x - s->x)) < GAP &&
		    abs((int)(o->y - s->y)) < GAP)
			return false;
	}
	return true;
}

/* Somewhere off the top or the right edge, clear of the others (tried a
 * few times), to fly in from; or anywhere along the way, to start with. */
static void place(struct sprite *s, bool anywhere)
{
	int tries;

	for (tries = 0; tries < 20; tries++) {
		if (anywhere) {
			s->x = rand() % (W + H) - SZ;
			s->y = rand() % (H + SZ) - SZ - (rand() % H);
		} else if (rand() % (W + H) < W) {	/* along the top */
			s->x = rand() % (W + SZ);
			s->y = -SZ - rand() % (H / 2);
		} else {				/* down the right */
			s->x = W + rand() % (W / 2);
			s->y = rand() % (H * 3 / 4) - SZ;
		}
		if (clear_of_others(s))
			break;
	}
}

static void tick(lv_timer_t *t)
{
	uint32_t now = lv_tick_get(), dt = now - last_ms;
	int i;

	(void)t;
	if (moved)
		return;		/* (the time adds up: no slower for it) */
	moved = true;
	last_ms = now;
	if (dt > 200)
		dt = 200;	/* (stopped a while: don't jump) */
	for (i = 0; i < N; i++) {
		struct sprite *s = &sprites[i];

		s->x -= s->v * dt / 1000.0f;
		s->y += s->v * dt / 1000.0f;
		if (s->x < -SZ || s->y > H)	/* gone: in again */
			place(s, false);
		lv_obj_set_pos(s->img, (int32_t)s->x, (int32_t)s->y);
		if (s->toaster) {
			/* 0 1 2 3 3 2 1 0: forward and back, as the CSS's
			 * steps(4) alternate */
			int step = (int)((now + s->phase) / FLAP_STEP_MS % (2 * FRAMES));
			int frame = step < FRAMES ? step : 2 * FRAMES - 1 - step;

			if (frame != s->frame) {
				s->frame = frame;
				lv_image_set_src(s->img, wings[frame]);
			}
		}
	}
}

static void drawn(lv_event_t *e)
{
	(void)e;
	moved = false;
}

static void tapped(lv_event_t *e)
{
	(void)e;
	exit(0);
}

int main(void)
{
	struct gif sheet = { 0 }, slice = { 0 };
	lv_display_t *disp;
	lv_obj_t *scr;
	int i;

	chumby_init();		/* (LVGL first: the frames are its images) */
	if (!gif_load(DIR "/toaster-sprite.gif", &sheet) || !gif_load(DIR "/toast1.gif", &slice)) {
		fprintf(stderr, "chumby-toasters: can't read the pictures in " DIR "\n");
		return 1;
	}
	for (i = 0; i < FRAMES; i++)
		if (!(wings[i] = frame_make(&sheet, i * SRC)))
			return 1;
	if (!(toast = frame_make(&slice, 0)))
		return 1;

	/* See-through: chumby-wm blurs and dims what's under us. */
	lv_wayland_window_set_next_color_format(LV_COLOR_FORMAT_ARGB8888);
	disp = chumby_window_create("chumby-saver", W, H);
	if (!disp)
		return 1;
	scr = lv_display_get_screen_active(disp);
	lv_obj_clean(scr);
	lv_obj_set_scrollable(scr, false);
	lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
	lv_obj_add_event_cb(scr, tapped, LV_EVENT_PRESSED, NULL);

	srand(time(NULL));
	for (i = 0; i < N; i++) {
		struct sprite *s = &sprites[i];

		s->toaster = i < N_TOASTERS;
		s->v = speeds[i % 3];
		s->phase = rand() % (2 * FRAMES * FLAP_STEP_MS);
		s->frame = -1;
		place(s, true);
		s->img = lv_image_create(scr);
		lv_obj_set_clickable(s->img, false);
		lv_image_set_src(s->img, s->toaster ? wings[0] : toast);
		lv_obj_set_pos(s->img, (int32_t)s->x, (int32_t)s->y);
	}
	last_ms = lv_tick_get();
	lv_display_add_event_cb(disp, drawn, LV_EVENT_REFR_READY, NULL);
	lv_timer_create(tick, TICK_MS, NULL);
	return chumby_run();
}
