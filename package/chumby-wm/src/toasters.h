/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Flying Toasters, drawn by chumby-wm (toasters.c). */
#ifndef CHUMBY_WM_TOASTERS_H
#define CHUMBY_WM_TOASTERS_H

#include <stdbool.h>
#include <stdint.h>

struct toasters;
struct wlr_scene_tree;
struct wl_event_loop;

/* Flying over a w x h area, in a tree of their own under parent, moved
 * every tick_ms: over what's under them, or (canvas: no GPU) drawn onto
 * one canvas in format (the screen's DRM format) over backdrop (XRGB8888,
 * w x h, ours to free; NULL: black). NULL if the pictures can't be read. */
struct toasters *toasters_start(struct wlr_scene_tree *parent, struct wl_event_loop *loop,
				int w, int h, int tick_ms, bool canvas, uint32_t format,
				uint32_t *backdrop);
void toasters_stop(struct toasters *t);

#endif
