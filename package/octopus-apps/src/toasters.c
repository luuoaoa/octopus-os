// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * octopus-toasters: Flying Toasters, the After Dark screen saver - a
 * screen saver on liboctopus's saver kit (octopus_saver.h), installed as
 * /usr/lib/octopus/savers/toasters.
 *
 * After Dark's toasters and toast (as After Dark CSS draws them,
 * https://www.bryanbraun.com/after-dark-css/) glide down from the top
 * right to the bottom left at three speeds, the toasters flapping their
 * wings. Five pictures (four wing frames, the toast) are made once; a
 * frame puts the backdrop back where each sprite was, draws it where it
 * is, and says those squares changed - all the kit shows. (As an LVGL app
 * the same saver cost the Chumby's ARM9 ~40%: LVGL's per-object, per-area
 * bookkeeping. Drawn this way, by octopus-wm for a while, ~12%.)
 *
 * The backdrop is the kit's: with a GPU see-through (octopus-wm blurs the
 * screen under it), else the screen as the saver came up, blurred and
 * dimmed. 25 frames a second with a GPU, 12.5 on the ARM9.
 *
 * The pictures are After Dark CSS's GIFs (toaster-sprite.gif: four 64 px
 * wing frames side by side; toast1.gif), (c) Berkeley Systems: fetched
 * when the image is built (package/after-dark-toasters), decoded here and
 * shown at half size, as big against our 320x240 as against After Dark's
 * 640x480.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <octopus_saver.h>

#define DIR		"/usr/share/octopus/toasters"
#define SRC		64		/* the GIFs' frame size */
#define SZ		32		/* ours */
#define FRAMES		4
#define N_TOASTERS	8
#define N_TOAST		4
#define N		(N_TOASTERS + N_TOAST)
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

/* ------------------------------------------------------------------ */
/* Pictures: premultiplied ARGB                                       */

/* A SRC x SRC square of the GIF, at x0, halved (2x2 averaged, by alpha:
 * the outlines stay), premultiplied as pixman wants it. */
static pixman_image_t *pic_make(const struct gif *g, int x0)
{
	uint32_t *px = malloc(sizeof(uint32_t) * SZ * SZ);
	int x, y, k;

	if (!px)
		return NULL;
	for (y = 0; y < SZ; y++)
		for (x = 0; x < SZ; x++) {
			unsigned a = 0, r = 0, gr = 0, bl = 0;

			for (k = 0; k < 4; k++) {
				int sx = x0 + 2 * x + (k & 1), sy = 2 * y + (k >> 1);
				uint32_t v = sx < g->w && sy < g->h ? g->argb[sy * g->w + sx] : 0;
				unsigned pa = v >> 24;

				a += pa;
				r += (v >> 16 & 0xff) * pa;
				gr += (v >> 8 & 0xff) * pa;
				bl += (v & 0xff) * pa;
			}
			/* sum(c * a) / 4 / 255: premultiplied by the averaged alpha */
			px[y * SZ + x] = (a / 4) << 24 | (r / 1020) << 16 | (gr / 1020) << 8 | bl / 1020;
		}
	return pixman_image_create_bits(PIXMAN_a8r8g8b8, SZ, SZ, px, SZ * 4);
}

static pixman_image_t *wings[FRAMES], *toast;

static bool pics_load(void)
{
	struct gif sheet = { 0 }, slice = { 0 };
	bool ok = false;
	int i;

	if (!gif_load(DIR "/toaster-sprite.gif", &sheet) || !gif_load(DIR "/toast1.gif", &slice)) {
		fprintf(stderr, "octopus-toasters: can't read the pictures in " DIR "\n");
		goto out;
	}
	for (i = 0; i < FRAMES; i++)
		if (!(wings[i] = pic_make(&sheet, i * SRC)))
			goto out;
	toast = pic_make(&slice, 0);
	ok = toast != NULL;
out:
	free(sheet.argb);
	free(slice.argb);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Flying                                                             */

struct sprite {
	bool placed;
	bool toaster;
	float x, y, v;		/* top left; px/s down and left alike */
	int phase;		/* the wings: where in their beat */
	int frame;		/* shown */
	int dx, dy;		/* where it was drawn */
};

static struct sprite sprites[N];
static int scr_w, scr_h;
static uint64_t last_ms;

/* After Dark CSS's three speeds (1600 px in 10, 16, 24 s), at our size. */
static const float speeds[] = { 40.0f, 25.0f, 17.0f };

static bool clear_of_others(const struct sprite *s)
{
	int i;

	for (i = 0; i < N; i++) {
		const struct sprite *o = &sprites[i];

		if (o != s && o->placed && abs((int)(o->x - s->x)) < GAP &&
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
			s->x = rand() % (scr_w + scr_h) - SZ;
			s->y = rand() % (scr_h + SZ) - SZ - (rand() % scr_h);
		} else if (rand() % (scr_w + scr_h) < scr_w) {	/* along the top */
			s->x = rand() % (scr_w + SZ);
			s->y = -SZ - rand() % (scr_h / 2);
		} else {					/* down the right */
			s->x = scr_w + rand() % (scr_w / 2);
			s->y = rand() % (scr_h * 3 / 4) - SZ;
		}
		if (clear_of_others(s))
			break;
	}
	s->placed = true;
}

static void frame(struct octopus_saver *sv, uint64_t now, void *user)
{
	pixman_image_t *canvas = octopus_saver_canvas(sv);
	uint64_t dt = last_ms ? now - last_ms : 0;
	int i;

	(void)user;
	last_ms = now;
	if (dt > 200)
		dt = 200;	/* (stopped a while: don't jump) */
	/* Every sprite's old square back to the backdrop first, then all
	 * drawn: one that overlaps another isn't erased by it. */
	for (i = 0; i < N; i++)
		octopus_saver_erase(sv, sprites[i].dx, sprites[i].dy, SZ, SZ);
	for (i = 0; i < N; i++) {
		struct sprite *s = &sprites[i];

		s->x -= s->v * dt / 1000.0f;
		s->y += s->v * dt / 1000.0f;
		if (s->x < -SZ || s->y > scr_h)	/* gone: in again */
			place(s, false);
		if (s->toaster) {
			/* 0 1 2 3 3 2 1 0: forward and back, as the CSS's
			 * steps(4) alternate */
			int step = (int)((now + s->phase) / FLAP_STEP_MS % (2 * FRAMES));

			s->frame = step < FRAMES ? step : 2 * FRAMES - 1 - step;
		}
		s->dx = (int)s->x;
		s->dy = (int)s->y;
		pixman_image_composite32(PIXMAN_OP_OVER, s->toaster ? wings[s->frame] : toast,
					 NULL, canvas, 0, 0, 0, 0, s->dx, s->dy, SZ, SZ);
		octopus_saver_damage(sv, s->dx, s->dy, SZ, SZ);
	}
}

int main(void)
{
	struct octopus_saver *sv;
	int i;

	if (!pics_load())
		return 1;
	sv = octopus_saver_open(&(struct octopus_saver_opts){
		.tick_ms = octopus_saver_gpu() ? 40 : 80 });
	if (!sv)
		return 1;
	scr_w = octopus_saver_width(sv);
	scr_h = octopus_saver_height(sv);
	srand(time(NULL));
	for (i = 0; i < N; i++) {
		struct sprite *s = &sprites[i];

		s->toaster = i < N_TOASTERS;
		s->v = speeds[i % 3];
		s->phase = rand() % (2 * FRAMES * FLAP_STEP_MS);
		place(s, true);
		s->dx = (int)s->x;
		s->dy = (int)s->y;
	}
	return octopus_saver_run(sv, frame, NULL);
}
