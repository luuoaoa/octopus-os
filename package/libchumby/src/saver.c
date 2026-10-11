// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The screen saver kit (chumby_saver.h): a Wayland window of our own, no
 * LVGL - its own connection, two shared-memory buffers, a private canvas
 * the saver draws on, and only the changed squares copied and committed.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "chumby.h"
#include "chumby_saver.h"
#include "wayland_xdg_shell.h"
#include "wayland_screencopy.h"

#define RENDER_NODE	"/dev/dri/renderD128"	/* a GPU: see-through */
#define APP_ID		"chumby-saver"	/* chumby-wm: the saver, over everything */
#define BLUR_R		4		/* box radius; three passes ~ Gaussian (as chumby-wm's) */
#define BLUR_PASSES	3
#define DIM		0.45f		/* black over the blurred screen (as the GPU's) */

struct buf {
	struct wl_buffer *wl;
	uint32_t *px;
	pixman_image_t *img;
	bool busy;			/* the compositor has it */
	pixman_region32_t stale;	/* behind the canvas here */
};

struct chumby_saver {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_output *output;
	struct zwlr_screencopy_manager_v1 *screencopy;
	struct wl_touch *touch;
	struct wl_pointer *pointer;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;

	int w, h, tick_ms;
	bool see_through, configured, frame_pending;
	struct buf bufs[2];
	uint32_t *canvas_px, *backdrop_px;	/* backdrop: NULL when see-through or black */
	pixman_image_t *canvas, *backdrop;
	pixman_region32_t pending;	/* changed since the last commit */

	/* The screen, captured before our window shows (screencopy). */
	struct {
		uint32_t format, w, h, stride;
		void *data;
		size_t size;
		struct wl_buffer *wl;
		bool y_invert, done, failed;
	} cap;
};

static volatile sig_atomic_t quit;
static bool gone;	/* the compositor */

static void on_term(int sig)
{
	(void)sig;
	quit = 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

bool chumby_saver_gpu(void)
{
	return access(RENDER_NODE, F_OK) == 0;
}

/* A shared-memory file of size bytes, mapped. */
static void *shm_map(size_t size, int *fd_out)
{
	int fd = memfd_create("chumby-saver", MFD_CLOEXEC);
	void *p;

	if (fd < 0)
		return NULL;
	if (ftruncate(fd, size) < 0 ||
	    (p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	*fd_out = fd;
	return p;
}

/* ------------------------------------------------------------------ */
/* Wayland plumbing                                                   */

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_ping };

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	struct chumby_saver *s = data;

	xdg_surface_ack_configure(xs, serial);
	s->configured = true;
}

static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };

static void tl_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
			 struct wl_array *states)
{
	struct chumby_saver *s = data;

	(void)t; (void)states;
	if (w > 0 && h > 0 && !s->bufs[0].wl) {	/* (the size is fixed once drawn) */
		s->w = w;
		s->h = h;
	}
}

static void tl_close(void *data, struct xdg_toplevel *t)
{
	(void)data; (void)t;
	quit = 1;
}

static const struct xdg_toplevel_listener tl_listener = {
	.configure = tl_configure,
	.close = tl_close,
};

/* Any touch or click ends it. */
static void touch_down(void *data, struct wl_touch *t, uint32_t serial, uint32_t time,
		       struct wl_surface *sf, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)t; (void)serial; (void)time; (void)sf; (void)id; (void)x; (void)y;
	quit = 1;
}
static void touch_up(void *d, struct wl_touch *t, uint32_t s, uint32_t tm, int32_t id)
{ (void)d; (void)t; (void)s; (void)tm; (void)id; }
static void touch_motion(void *d, struct wl_touch *t, uint32_t tm, int32_t id, wl_fixed_t x,
			 wl_fixed_t y)
{ (void)d; (void)t; (void)tm; (void)id; (void)x; (void)y; }
static void touch_frame(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void touch_cancel(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void touch_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t a, wl_fixed_t b)
{ (void)d; (void)t; (void)id; (void)a; (void)b; }
static void touch_orientation(void *d, struct wl_touch *t, int32_t id, wl_fixed_t o)
{ (void)d; (void)t; (void)id; (void)o; }

static const struct wl_touch_listener touch_listener = {
	.down = touch_down, .up = touch_up, .motion = touch_motion, .frame = touch_frame,
	.cancel = touch_cancel, .shape = touch_shape, .orientation = touch_orientation,
};

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf,
		      wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)s; (void)sf; (void)x; (void)y; }
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
{ (void)d; (void)p; (void)s; (void)sf; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)t; (void)x; (void)y; }
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t button,
		       uint32_t state)
{
	(void)d; (void)p; (void)s; (void)t; (void)button;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		quit = 1;
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t src)
{ (void)d; (void)p; (void)src; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
	.axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct chumby_saver *s = data;

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !s->touch) {
		s->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(s->touch, &touch_listener, s);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !s->pointer) {
		s->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(s->pointer, &pointer_listener, s);
	}
}

static void seat_name(void *d, struct wl_seat *seat, const char *name)
{ (void)d; (void)seat; (void)name; }

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps, .name = seat_name,
};

static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
		       uint32_t version)
{
	struct chumby_saver *s = data;

	if (!strcmp(iface, wl_compositor_interface.name) && version >= 4)
		s->compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		s->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		s->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(s->wm_base, &wm_base_listener, s);
	} else if (!strcmp(iface, wl_seat_interface.name) && !s->seat) {
		s->seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(s->seat, &seat_listener, s);
	} else if (!strcmp(iface, wl_output_interface.name) && !s->output)
		s->output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		s->screencopy = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }

static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

/* ------------------------------------------------------------------ */
/* The backdrop: the screen as the saver comes up                     */

static void cap_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
		       uint32_t w, uint32_t h, uint32_t stride)
{
	struct chumby_saver *s = data;
	struct wl_shm_pool *pool;
	int fd;

	s->cap.format = format;
	s->cap.w = w;
	s->cap.h = h;
	s->cap.stride = stride;
	s->cap.size = (size_t)stride * h;
	if (!(s->cap.data = shm_map(s->cap.size, &fd))) {
		s->cap.failed = true;
		return;
	}
	pool = wl_shm_create_pool(s->shm, fd, s->cap.size);
	s->cap.wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	zwlr_screencopy_frame_v1_copy(f, s->cap.wl);
}

static void cap_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
	struct chumby_saver *s = data;

	(void)f;
	s->cap.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void cap_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b,
		      uint32_t c)
{
	(void)f; (void)a; (void)b; (void)c;
	((struct chumby_saver *)data)->cap.done = true;
}

static void cap_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
	(void)f;
	((struct chumby_saver *)data)->cap.failed = true;
}

static const struct zwlr_screencopy_frame_v1_listener cap_listener = {
	.buffer = cap_buffer, .flags = cap_flags, .ready = cap_ready, .failed = cap_failed,
};

static bool capture(struct chumby_saver *s)
{
	struct zwlr_screencopy_frame_v1 *f;

	if (!s->screencopy || !s->output || !s->shm)
		return false;
	f = zwlr_screencopy_manager_v1_capture_output(s->screencopy, 0, s->output);
	zwlr_screencopy_frame_v1_add_listener(f, &cap_listener, s);
	while (!s->cap.done && !s->cap.failed && wl_display_dispatch(s->display) != -1)
		;
	zwlr_screencopy_frame_v1_destroy(f);
	if (s->cap.wl)
		wl_buffer_destroy(s->cap.wl);
	return s->cap.done;
}

/* One box-blur pass along a line of n pixels, `step` apart, all four
 * channels: a running sum over 2r+1, the edges repeated. */
static void blur_line(uint32_t *p, int n, int step, int r, uint32_t *tmp)
{
	uint32_t sum[4] = { 0 };
	int i, c, k = 2 * r + 1;

	for (i = 0; i < n; i++)
		tmp[i] = p[i * step];
	for (i = -r; i <= r; i++) {
		uint32_t v = tmp[i < 0 ? 0 : i >= n ? n - 1 : i];

		for (c = 0; c < 4; c++)
			sum[c] += v >> (c * 8) & 0xff;
	}
	for (i = 0; i < n; i++) {
		uint32_t out = 0, in = tmp[i + r + 1 < n ? i + r + 1 : n - 1],
			 gone = tmp[i - r > 0 ? i - r : 0];

		for (c = 0; c < 4; c++) {
			out |= (sum[c] / k) << (c * 8);
			sum[c] += (in >> (c * 8) & 0xff) - (gone >> (c * 8) & 0xff);
		}
		p[i * step] = out;
	}
}

/* The capture at our size (nearest: the output may be a whole multiple of
 * it), as XRGB, blurred and dimmed. */
static uint32_t *backdrop_make(struct chumby_saver *s)
{
	bool bgr = s->cap.format == WL_SHM_FORMAT_XBGR8888 ||
		   s->cap.format == WL_SHM_FORMAT_ABGR8888;
	uint32_t k = (uint32_t)((1 - DIM) * 256), *px, *tmp;
	int x, y, pass;

	if (s->cap.format != WL_SHM_FORMAT_XRGB8888 && s->cap.format != WL_SHM_FORMAT_ARGB8888 &&
	    !bgr) {
		fprintf(stderr, "chumby-saver: screen capture in format 0x%x: black instead\n",
			s->cap.format);
		return NULL;
	}
	px = malloc(sizeof(uint32_t) * s->w * s->h);
	tmp = malloc(sizeof(uint32_t) * (s->w > s->h ? s->w : s->h));
	if (!px || !tmp) {
		free(px);
		free(tmp);
		return NULL;
	}
	for (y = 0; y < s->h; y++) {
		uint32_t sy = (uint32_t)y * s->cap.h / s->h;
		const uint32_t *row = (const uint32_t *)((const uint8_t *)s->cap.data +
			(size_t)(s->cap.y_invert ? s->cap.h - 1 - sy : sy) * s->cap.stride);

		for (x = 0; x < s->w; x++) {
			uint32_t v = row[(uint32_t)x * s->cap.w / s->w];

			if (bgr)
				v = (v & 0xff00ff00u) | (v & 0xff) << 16 | (v >> 16 & 0xff);
			px[y * s->w + x] = v;
		}
	}
	for (pass = 0; pass < BLUR_PASSES; pass++) {
		for (y = 0; y < s->h; y++)
			blur_line(px + y * s->w, s->w, 1, BLUR_R, tmp);
		for (x = 0; x < s->w; x++)
			blur_line(px + x, s->h, s->w, BLUR_R, tmp);
	}
	free(tmp);
	for (x = 0; x < s->w * s->h; x++) {	/* dimmed: each channel x (1 - DIM) */
		uint32_t v = px[x];

		px[x] = 0xff000000u | (((v & 0xff00ffu) * k >> 8) & 0xff00ffu) |
			(((v & 0x00ff00u) * k >> 8) & 0x00ff00u);
	}
	return px;
}

/* ------------------------------------------------------------------ */
/* The window                                                         */

static void buf_release(void *data, struct wl_buffer *wl)
{
	(void)wl;
	((struct buf *)data)->busy = false;
}

static const struct wl_buffer_listener buf_listener = { .release = buf_release };

static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
	(void)t;
	((struct chumby_saver *)data)->frame_pending = false;
	wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static bool bufs_make(struct chumby_saver *s)
{
	int stride = s->w * 4, i, fd;
	size_t size = (size_t)stride * s->h;
	uint32_t format = s->see_through ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;

	for (i = 0; i < 2; i++) {
		struct buf *b = &s->bufs[i];
		struct wl_shm_pool *pool;

		if (!(b->px = shm_map(size, &fd)))
			return false;
		pool = wl_shm_create_pool(s->shm, fd, size);
		b->wl = wl_shm_pool_create_buffer(pool, 0, s->w, s->h, stride, format);
		wl_shm_pool_destroy(pool);
		close(fd);
		wl_buffer_add_listener(b->wl, &buf_listener, b);
		b->img = pixman_image_create_bits(s->see_through ? PIXMAN_a8r8g8b8 :
						  PIXMAN_x8r8g8b8, s->w, s->h, b->px, stride);
		pixman_region32_init_rect(&b->stale, 0, 0, s->w, s->h);
	}
	return true;
}

/* What changed, into a free buffer (with what it missed while the other
 * was up), committed with just that damaged. */
static void commit(struct chumby_saver *s)
{
	struct buf *b = !s->bufs[0].busy ? &s->bufs[0] : !s->bufs[1].busy ? &s->bufs[1] : NULL;
	struct buf *other;
	pixman_region32_t copy;
	pixman_box32_t *box;
	int i, n;

	if (!b || s->frame_pending || !pixman_region32_not_empty(&s->pending))
		return;
	other = b == &s->bufs[0] ? &s->bufs[1] : &s->bufs[0];
	pixman_region32_init(&copy);
	pixman_region32_union(&copy, &b->stale, &s->pending);
	box = pixman_region32_rectangles(&copy, &n);
	for (i = 0; i < n; i++)
		pixman_image_composite32(PIXMAN_OP_SRC, s->canvas, NULL, b->img,
					 box[i].x1, box[i].y1, 0, 0, box[i].x1, box[i].y1,
					 box[i].x2 - box[i].x1, box[i].y2 - box[i].y1);
	pixman_region32_fini(&copy);
	pixman_region32_clear(&b->stale);
	pixman_region32_union(&other->stale, &other->stale, &s->pending);

	wl_surface_attach(s->surface, b->wl, 0, 0);
	box = pixman_region32_rectangles(&s->pending, &n);
	for (i = 0; i < n; i++)
		wl_surface_damage_buffer(s->surface, box[i].x1, box[i].y1,
					 box[i].x2 - box[i].x1, box[i].y2 - box[i].y1);
	wl_callback_add_listener(wl_surface_frame(s->surface), &frame_listener, s);
	wl_surface_commit(s->surface);
	b->busy = true;
	s->frame_pending = true;
	pixman_region32_clear(&s->pending);
}

struct chumby_saver *chumby_saver_open(const struct chumby_saver_opts *opts)
{
	struct chumby_saver *s = calloc(1, sizeof(*s));
	struct sigaction sa = { .sa_handler = on_term };	/* no SA_RESTART: poll wakes */
	uint64_t t0;

	if (!s)
		return NULL;
	s->tick_ms = opts && opts->tick_ms > 0 ? opts->tick_ms : 80;
	s->see_through = chumby_saver_gpu();
	s->w = CHUMBY_SCREEN_W;
	s->h = CHUMBY_SCREEN_H;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	if (!(s->display = wl_display_connect(NULL))) {
		fprintf(stderr, "chumby-saver: no Wayland display\n");
		return NULL;
	}
	wl_registry_add_listener(wl_display_get_registry(s->display), &reg_listener, s);
	wl_display_roundtrip(s->display);
	if (!s->compositor || !s->shm || !s->wm_base) {
		fprintf(stderr, "chumby-saver: compositor lacks wl_compositor 4/wl_shm/xdg_wm_base\n");
		return NULL;
	}

	/* The screen, before our window covers it. */
	t0 = now_ms();
	if (!s->see_through && !(opts && opts->black) && !capture(s))
		fprintf(stderr, "chumby-saver: no screen capture: black instead\n");

	s->surface = wl_compositor_create_surface(s->compositor);
	s->xdg_surface = xdg_wm_base_get_xdg_surface(s->wm_base, s->surface);
	xdg_surface_add_listener(s->xdg_surface, &xs_listener, s);
	s->toplevel = xdg_surface_get_toplevel(s->xdg_surface);
	xdg_toplevel_add_listener(s->toplevel, &tl_listener, s);
	xdg_toplevel_set_app_id(s->toplevel, APP_ID);
	xdg_toplevel_set_title(s->toplevel, "Screen saver");
	wl_surface_commit(s->surface);
	while (!s->configured && wl_display_dispatch(s->display) != -1)
		;

	s->canvas_px = calloc((size_t)s->w * s->h, 4);	/* black, or transparent */
	if (!s->canvas_px || !bufs_make(s))
		return NULL;
	s->canvas = pixman_image_create_bits(s->see_through ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8,
					     s->w, s->h, s->canvas_px, s->w * 4);
	if (s->cap.done && (s->backdrop_px = backdrop_make(s))) {
		s->backdrop = pixman_image_create_bits(PIXMAN_x8r8g8b8, s->w, s->h,
						       s->backdrop_px, s->w * 4);
		memcpy(s->canvas_px, s->backdrop_px, sizeof(uint32_t) * s->w * s->h);
	}
	if (s->cap.data)
		munmap(s->cap.data, s->cap.size);
	s->cap.data = NULL;
	if (s->backdrop)
		fprintf(stderr, "chumby-saver: backdrop in %llu ms\n",
			(unsigned long long)(now_ms() - t0));
	pixman_region32_init_rect(&s->pending, 0, 0, s->w, s->h);
	return s;
}

int chumby_saver_width(const struct chumby_saver *s) { return s->w; }
int chumby_saver_height(const struct chumby_saver *s) { return s->h; }
bool chumby_saver_see_through(const struct chumby_saver *s) { return s->see_through; }
pixman_image_t *chumby_saver_canvas(struct chumby_saver *s) { return s->canvas; }

void chumby_saver_damage(struct chumby_saver *s, int x, int y, int w, int h)
{
	pixman_region32_t r;

	pixman_region32_init_rect(&r, x, y, w, h);
	pixman_region32_intersect_rect(&r, &r, 0, 0, s->w, s->h);
	pixman_region32_union(&s->pending, &s->pending, &r);
	pixman_region32_fini(&r);
}

void chumby_saver_erase(struct chumby_saver *s, int x, int y, int w, int h)
{
	static const pixman_color_t clear = { 0, 0, 0, 0 }, black = { 0, 0, 0, 0xffff };
	pixman_box32_t box;

	/* (Clipped here: pixman fills boxes as given, off the canvas too.) */
	box.x1 = x < 0 ? 0 : x;
	box.y1 = y < 0 ? 0 : y;
	box.x2 = x + w > s->w ? s->w : x + w;
	box.y2 = y + h > s->h ? s->h : y + h;
	if (box.x1 >= box.x2 || box.y1 >= box.y2)
		return;
	if (s->backdrop)
		pixman_image_composite32(PIXMAN_OP_SRC, s->backdrop, NULL, s->canvas,
					 box.x1, box.y1, 0, 0, box.x1, box.y1,
					 box.x2 - box.x1, box.y2 - box.y1);
	else
		pixman_image_fill_boxes(PIXMAN_OP_SRC, s->canvas,
					s->see_through ? &clear : &black, 1, &box);
	chumby_saver_damage(s, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1);
}

int chumby_saver_run(struct chumby_saver *s, chumby_saver_frame_cb frame, void *user)
{
	struct pollfd pfd = { .fd = wl_display_get_fd(s->display), .events = POLLIN };
	uint64_t next = now_ms();

	while (!quit && !gone) {
		uint64_t now = now_ms();
		int timeout = next > now ? (int)(next - now) : 0;

		while (wl_display_prepare_read(s->display) != 0)
			wl_display_dispatch_pending(s->display);
		wl_display_flush(s->display);
		if (poll(&pfd, 1, timeout) > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_read_events(s->display) < 0)
				gone = true;
		} else {
			wl_display_cancel_read(s->display);
		}
		if (pfd.revents & (POLLHUP | POLLERR))
			gone = true;
		if (wl_display_dispatch_pending(s->display) < 0)
			gone = true;

		now = now_ms();
		if (now >= next) {
			frame(s, now, user);
			next += s->tick_ms;
			if (next <= now)	/* (fell behind: no catching up) */
				next = now + s->tick_ms;
		}
		commit(s);
	}
	return gone ? 1 : 0;
}
