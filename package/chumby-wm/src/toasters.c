// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Flying Toasters, drawn by chumby-wm: After Dark's toasters and toast (as
 * After Dark CSS draws them, https://www.bryanbraun.com/after-dark-css/)
 * gliding down from the top right to the bottom left at three speeds, the
 * toasters flapping their wings.
 *
 * Drawn here rather than by an app, from five pictures (four wing frames,
 * the toast) made once. Over the app (the GPU's way: see-through), each
 * sprite is a scene node showing one of them; a frame moves the nodes and
 * swaps the wing pictures, and wlroots composites the squares that
 * changed. Over black (no GPU), one screen-sized canvas instead: a frame
 * blacks out where the sprites were and draws them where they are
 * (pixman), and hands wlroots the canvas with just those squares damaged -
 * a dozen moving nodes cost the ARM9 more in wlroots' scene bookkeeping
 * (each move re-walks the tree, in soft floating point) than in pixels. As an LVGL app the same saver cost the Chumby's ARM9 ~40%
 * (LVGL's per-object, per-area bookkeeping; its 16-bit window converted for
 * the 32-bit screen; the two processes' frames). The chumby-toasters app
 * only holds the screen (its window, see-through or black) and ends at a
 * tap; while its window is up, we fly over it.
 *
 * The pictures are After Dark CSS's GIFs (toaster-sprite.gif: four 64 px
 * wing frames side by side; toast1.gif), (c) Berkeley Systems: fetched
 * when the image is built (package/after-dark-toasters), decoded here and
 * shown at half size, as big against our 320x240 as against After Dark's
 * 640x480.
 */

#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <pixman.h>
#include "toasters.h"

#define DIR		"/usr/share/chumby/toasters"
#define SRC		64		/* the GIFs' frame size */
#define SZ		32		/* ours */
#define FRAMES		4
#define N_TOASTERS	8
#define N_TOAST		4
#define N		(N_TOASTERS + N_TOAST)
#define FLAP_STEP_MS	50		/* .2 s for the four frames, then back */
#define GAP		(SZ + 4)	/* apart, at least, when they come in */

/* ------------------------------------------------------------------ */
/* GIF (the app's reader, as it was)                                  */

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
/* Pictures: premultiplied ARGB buffers                               */

struct pic {
	struct wlr_buffer base;
	pixman_image_t *img;	/* the same pixels, for drawing on the canvas */
	uint32_t px[SZ * SZ];
};

static void pic_destroy(struct wlr_buffer *b)
{
	struct pic *p = wl_container_of(b, p, base);

	if (p->img)
		pixman_image_unref(p->img);
	free(p);
}

static bool pic_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format,
		      size_t *stride)
{
	struct pic *p = wl_container_of(b, p, base);

	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = p->px;
	*format = DRM_FORMAT_ARGB8888;
	*stride = SZ * 4;
	return true;
}

static void pic_end(struct wlr_buffer *b)
{
	(void)b;
}

static const struct wlr_buffer_impl pic_impl = {
	.destroy = pic_destroy,
	.begin_data_ptr_access = pic_begin,
	.end_data_ptr_access = pic_end,
};

/* A SRC x SRC square of the GIF, at x0, halved (2x2 averaged, by alpha:
 * the outlines stay), premultiplied as wlroots wants it. */
static struct wlr_buffer *pic_make(const struct gif *g, int x0)
{
	struct pic *p = calloc(1, sizeof(*p));
	int x, y, k;

	if (!p)
		return NULL;
	wlr_buffer_init(&p->base, &pic_impl, SZ, SZ);
	for (y = 0; y < SZ; y++)
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
			/* sum(c * a) / 4 / 255: premultiplied by the averaged alpha */
			p->px[y * SZ + x] = (a / 4) << 24 | (r / 1020) << 16 | (gr / 1020) << 8 | bl / 1020;
		}
	p->img = pixman_image_create_bits(PIXMAN_a8r8g8b8, SZ, SZ, p->px, SZ * 4);
	return &p->base;
}

/* Made once, kept (a few KB): the four wing frames and the toast. */
static struct wlr_buffer *wings[FRAMES], *toast;

static bool pics_load(void)
{
	struct gif sheet = { 0 }, slice = { 0 };
	bool ok = false;
	int i;

	if (toast)
		return true;
	if (!gif_load(DIR "/toaster-sprite.gif", &sheet) || !gif_load(DIR "/toast1.gif", &slice)) {
		fprintf(stderr, "chumby-wm: toasters: can't read the pictures in " DIR "\n");
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
	struct wlr_scene_buffer *node;	/* over the app; else on the canvas */
	bool placed;
	bool toaster;
	float x, y, v;		/* top left; px/s down and left alike */
	int phase;		/* the wings: where in their beat */
	int frame;		/* shown */
	int dx, dy;		/* where it was drawn on the canvas */
};

/* Over black: the screen, in its format (a plain copy to show). */
struct canvas {
	struct wlr_buffer base;
	pixman_image_t *img;
	void *px;
	uint32_t format;	/* DRM's */
	int stride;
};

static void canvas_destroy(struct wlr_buffer *b)
{
	struct canvas *c = wl_container_of(b, c, base);

	pixman_image_unref(c->img);
	free(c->px);
	free(c);
}

static bool canvas_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format,
			 size_t *stride)
{
	struct canvas *c = wl_container_of(b, c, base);

	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = c->px;
	*format = c->format;
	*stride = c->stride;
	return true;
}

static const struct wlr_buffer_impl canvas_impl = {
	.destroy = canvas_destroy,
	.begin_data_ptr_access = canvas_begin,
	.end_data_ptr_access = pic_end,
};

static struct canvas *canvas_make(int w, int h, uint32_t format)
{
	struct canvas *c = calloc(1, sizeof(*c));
	bool rgb565 = format == DRM_FORMAT_RGB565;

	if (c) {
		c->format = rgb565 ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888;
		c->stride = (rgb565 ? 2 * w + 3 : 4 * w) & ~3;	/* (pixman's: whole words) */
	}
	if (!c || !(c->px = calloc(h, c->stride)) ||
	    !(c->img = pixman_image_create_bits(rgb565 ? PIXMAN_r5g6b5 : PIXMAN_x8r8g8b8,
						w, h, c->px, c->stride))) {
		if (c)
			free(c->px);
		free(c);
		return NULL;
	}
	wlr_buffer_init(&c->base, &canvas_impl, w, h);
	return c;
}

struct toasters {
	struct wlr_scene_tree *tree;
	struct wl_event_source *timer;
	int w, h, tick_ms;
	struct sprite s[N];
	struct canvas *canvas;		/* over black */
	struct wlr_scene_buffer *canvas_node;
	uint64_t last_ms;
};

/* After Dark CSS's three speeds (1600 px in 10, 16, 24 s), at our size. */
static const float speeds[] = { 40.0f, 25.0f, 17.0f };

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

static bool clear_of_others(struct toasters *t, const struct sprite *s)
{
	int i;

	for (i = 0; i < N; i++) {
		const struct sprite *o = &t->s[i];

		if (o != s && o->placed && abs((int)(o->x - s->x)) < GAP &&
		    abs((int)(o->y - s->y)) < GAP)
			return false;
	}
	return true;
}

/* Somewhere off the top or the right edge, clear of the others (tried a
 * few times), to fly in from; or anywhere along the way, to start with. */
static void place(struct toasters *t, struct sprite *s, bool anywhere)
{
	int tries;

	for (tries = 0; tries < 20; tries++) {
		if (anywhere) {
			s->x = rand() % (t->w + t->h) - SZ;
			s->y = rand() % (t->h + SZ) - SZ - (rand() % t->h);
		} else if (rand() % (t->w + t->h) < t->w) {	/* along the top */
			s->x = rand() % (t->w + SZ);
			s->y = -SZ - rand() % (t->h / 2);
		} else {					/* down the right */
			s->x = t->w + rand() % (t->w / 2);
			s->y = rand() % (t->h * 3 / 4) - SZ;
		}
		if (clear_of_others(t, s))
			break;
	}
	s->placed = true;
}

static struct wlr_buffer *picture(const struct sprite *s)
{
	return s->toaster ? wings[s->frame] : toast;
}

/* The canvas: black where the sprites were, them where they are, and
 * wlroots told just that much changed. */
static void canvas_draw(struct toasters *t)
{
	static const pixman_color_t black = { 0, 0, 0, 0xffff };
	pixman_image_t *dst = t->canvas->img;
	pixman_region32_t was, damage;
	pixman_box32_t *box;
	int i, n;

	pixman_region32_init(&was);
	pixman_region32_init(&damage);
	for (i = 0; i < N; i++) {
		struct sprite *s = &t->s[i];

		pixman_region32_union_rect(&was, &was, s->dx, s->dy, SZ, SZ);
		s->dx = (int)s->x;
		s->dy = (int)s->y;
		pixman_region32_union_rect(&damage, &damage, s->dx, s->dy, SZ, SZ);
	}
	/* (Clipped first: pixman fills boxes as given, off the canvas too.) */
	pixman_region32_intersect_rect(&was, &was, 0, 0, t->w, t->h);
	box = pixman_region32_rectangles(&was, &n);
	pixman_image_fill_boxes(PIXMAN_OP_SRC, dst, &black, n, box);
	for (i = 0; i < N; i++) {
		struct sprite *s = &t->s[i];
		struct pic *p = wl_container_of(picture(s), p, base);

		pixman_image_composite32(PIXMAN_OP_OVER, p->img, NULL, dst,
					 0, 0, 0, 0, s->dx, s->dy, SZ, SZ);
	}
	pixman_region32_union(&damage, &damage, &was);
	pixman_region32_intersect_rect(&damage, &damage, 0, 0, t->w, t->h);
	wlr_scene_buffer_set_buffer_with_damage(t->canvas_node, &t->canvas->base, &damage);
	pixman_region32_fini(&was);
	pixman_region32_fini(&damage);
}

static int tick(void *data)
{
	struct toasters *t = data;
	uint64_t now = now_ms(), dt = now - t->last_ms;
	int i;

	t->last_ms = now;
	if (dt > 200)
		dt = 200;	/* (stopped a while: don't jump) */
	for (i = 0; i < N; i++) {
		struct sprite *s = &t->s[i];

		s->x -= s->v * dt / 1000.0f;
		s->y += s->v * dt / 1000.0f;
		if (s->x < -SZ || s->y > t->h)	/* gone: in again */
			place(t, s, false);
		if (s->node)
			wlr_scene_node_set_position(&s->node->node, (int)s->x, (int)s->y);
		if (s->toaster) {
			/* 0 1 2 3 3 2 1 0: forward and back, as the CSS's
			 * steps(4) alternate */
			int step = (int)((now + s->phase) / FLAP_STEP_MS % (2 * FRAMES));
			int frame = step < FRAMES ? step : 2 * FRAMES - 1 - step;

			if (frame != s->frame) {
				s->frame = frame;
				if (s->node)
					wlr_scene_buffer_set_buffer(s->node, wings[frame]);
			}
		}
	}
	if (t->canvas)
		canvas_draw(t);
	wl_event_source_timer_update(t->timer, t->tick_ms);
	return 0;
}

struct toasters *toasters_start(struct wlr_scene_tree *parent, struct wl_event_loop *loop,
				int w, int h, int tick_ms, bool black, uint32_t format)
{
	struct toasters *t;
	int i;

	if (!pics_load() || !(t = calloc(1, sizeof(*t))))
		return NULL;
	t->tree = wlr_scene_tree_create(parent);
	/* Black: the canvas (opaque: what's under it is left out). */
	if (black) {
		if (!(t->canvas = canvas_make(w, h, format))) {
			wlr_scene_node_destroy(&t->tree->node);
			free(t);
			return NULL;
		}
		t->canvas_node = wlr_scene_buffer_create(t->tree, &t->canvas->base);
		wlr_scene_buffer_set_filter_mode(t->canvas_node, WLR_SCALE_FILTER_NEAREST);
	}
	t->w = w;
	t->h = h;
	t->tick_ms = tick_ms;
	srand(time(NULL));
	for (i = 0; i < N; i++) {
		struct sprite *s = &t->s[i];

		s->toaster = i < N_TOASTERS;
		s->v = speeds[i % 3];
		s->phase = rand() % (2 * FRAMES * FLAP_STEP_MS);
		s->frame = 0;
		place(t, s, true);
		s->dx = (int)s->x;
		s->dy = (int)s->y;
		if (t->canvas)
			continue;
		s->node = wlr_scene_buffer_create(t->tree, picture(s));
		/* Scaled up (a bigger screen): crisp pixels, as the UI's. */
		wlr_scene_buffer_set_filter_mode(s->node, WLR_SCALE_FILTER_NEAREST);
		wlr_scene_node_set_position(&s->node->node, s->dx, s->dy);
	}
	if (t->canvas)
		canvas_draw(t);
	t->last_ms = now_ms();
	t->timer = wl_event_loop_add_timer(loop, tick, t);
	wl_event_source_timer_update(t->timer, tick_ms);
	return t;
}

void toasters_stop(struct toasters *t)
{
	if (!t)
		return;
	wl_event_source_remove(t->timer);
	wlr_scene_node_destroy(&t->tree->node);
	if (t->canvas)
		wlr_buffer_drop(&t->canvas->base);
	free(t);
}
