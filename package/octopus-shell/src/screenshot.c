// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * octopus-screenshot: save the screen as a binary PPM (P6), using
 * wlr-screencopy. Usage: octopus-screenshot [-c] [file.ppm]  (default: stdout;
 * -c: with the mouse cursor in it)
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm *shm;
static struct wl_output *output;
static struct zwlr_screencopy_manager_v1 *manager;

static struct {
	uint32_t format, width, height, stride;
	void *data;
	struct wl_buffer *buffer;
	bool done, failed, y_invert;
} frame;

static void frame_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
			 uint32_t width, uint32_t height, uint32_t stride)
{
	size_t size = (size_t)stride * height;
	int fd = memfd_create("octopus-screenshot", MFD_CLOEXEC);
	struct wl_shm_pool *pool;

	(void)d;
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("octopus-screenshot: memfd");
		exit(1);
	}
	frame.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (frame.data == MAP_FAILED) {
		perror("octopus-screenshot: mmap");
		exit(1);
	}
	frame.format = format;
	frame.width = width;
	frame.height = height;
	frame.stride = stride;
	pool = wl_shm_create_pool(shm, fd, size);
	frame.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	zwlr_screencopy_frame_v1_copy(f, frame.buffer);
}

static void frame_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
	(void)d; (void)f;
	frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_ready(void *d, struct zwlr_screencopy_frame_v1 *f,
			uint32_t hi, uint32_t lo, uint32_t ns)
{
	(void)d; (void)f; (void)hi; (void)lo; (void)ns;
	frame.done = true;
}

static void frame_failed(void *d, struct zwlr_screencopy_frame_v1 *f)
{
	(void)d; (void)f;
	frame.failed = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
};

static void global(void *d, struct wl_registry *r, uint32_t name,
		   const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		manager = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, 1);
}

static void global_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = global,
	.global_remove = global_remove,
};

int main(int argc, char **argv)
{
	int cursor = argc > 1 && !strcmp(argv[1], "-c");

	struct wl_display *wl = wl_display_connect(NULL);
	struct zwlr_screencopy_frame_v1 *f;
	FILE *out = stdout;
	uint32_t x, y;
	bool bgr;

	if (!wl) {
		fprintf(stderr, "octopus-screenshot: no Wayland display\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(wl), &registry_listener, NULL);
	wl_display_roundtrip(wl);
	if (!shm || !output || !manager) {
		fprintf(stderr, "octopus-screenshot: compositor lacks wl_shm/wl_output/screencopy\n");
		return 1;
	}

	f = zwlr_screencopy_manager_v1_capture_output(manager, cursor, output);
	zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, NULL);
	while (!frame.done && !frame.failed && wl_display_dispatch(wl) != -1)
		;
	if (!frame.done) {
		fprintf(stderr, "octopus-screenshot: capture failed\n");
		return 1;
	}
	/* XRGB from pixman (RGB565 on the Chumby's 16-bit screen), XBGR
	 * from the GPU (gles2). */
	if (frame.format != WL_SHM_FORMAT_XRGB8888 && frame.format != WL_SHM_FORMAT_ARGB8888 &&
	    frame.format != WL_SHM_FORMAT_XBGR8888 && frame.format != WL_SHM_FORMAT_ABGR8888 &&
	    frame.format != WL_SHM_FORMAT_RGB565) {
		fprintf(stderr, "octopus-screenshot: unsupported format 0x%x\n", frame.format);
		return 1;
	}

	if (cursor) {
		argc--;
		argv++;
	}
	if (argc > 1 && !(out = fopen(argv[1], "wb"))) {
		perror(argv[1]);
		return 1;
	}
	bgr = frame.format == WL_SHM_FORMAT_XBGR8888 || frame.format == WL_SHM_FORMAT_ABGR8888;
	fprintf(out, "P6\n%u %u\n255\n", frame.width, frame.height);
	for (y = 0; y < frame.height; y++) {
		uint32_t row = frame.y_invert ? frame.height - 1 - y : y;
		const uint8_t *p = (const uint8_t *)frame.data + (size_t)row * frame.stride;

		if (frame.format == WL_SHM_FORMAT_RGB565) {
			/* little-endian 16 bits, RRRRRGGG GGGBBBBB: each widened,
			 * its top bits repeated into the bottom ones */
			for (x = 0; x < frame.width; x++, p += 2) {
				unsigned px = p[0] | p[1] << 8;
				unsigned r = px >> 11, g = px >> 5 & 0x3f, b = px & 0x1f;

				fputc(r << 3 | r >> 2, out);
				fputc(g << 2 | g >> 4, out);
				fputc(b << 3 | b >> 2, out);
			}
			continue;
		}
		/* [X|A]RGB8888 little-endian: B, G, R, X in memory; [X|A]BGR8888
		 * the other way round: R, G, B, X */
		for (x = 0; x < frame.width; x++, p += 4) {
			fputc(p[bgr ? 0 : 2], out);
			fputc(p[1], out);
			fputc(p[bgr ? 2 : 0], out);
		}
	}
	if (out != stdout)
		fclose(out);
	return 0;
}
