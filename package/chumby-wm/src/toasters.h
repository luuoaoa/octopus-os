/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Flying Toasters, drawn by chumby-wm (toasters.c). */
#ifndef CHUMBY_WM_TOASTERS_H
#define CHUMBY_WM_TOASTERS_H

#include <stdbool.h>

struct toasters;
struct wlr_scene_tree;
struct wl_event_loop;

/* Flying over a w x h area, in a tree of their own under parent, moved
 * every tick_ms, over black (else over what's under them); NULL if the
 * pictures can't be read. */
struct toasters *toasters_start(struct wlr_scene_tree *parent, struct wl_event_loop *loop,
				int w, int h, int tick_ms, bool black);
void toasters_stop(struct toasters *t);

#endif
