/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The screen saver kit: what a screen saver needs and nothing else - no
 * LVGL (its per-object bookkeeping is what made the first saver cost the
 * Chumby's ARM9 ~40%).
 *
 * A saver is a program the shell starts after a while without input
 * (/usr/lib/chumby/savers/<name>, "saver = <name>" in policy.conf). It
 * draws with pixman on a canvas the size of the screen and says which
 * squares it changed; the kit shows them (two shared buffers, only the
 * changed squares copied and committed), paces the frames, and ends at a
 * tap, the compositor closing the window, or SIGTERM (the shell, at any
 * input). chumby-wm knows the window only as "the saver": over everything,
 * the app under it stopped, and on a GPU the screen under it blurred.
 *
 *   static void frame(struct chumby_saver *s, uint64_t now_ms, void *user)
 *   {
 *           chumby_saver_erase(s, old_x, old_y, 32, 32);   // backdrop back
 *           pixman_image_composite32(PIXMAN_OP_OVER, sprite, NULL,
 *                                    chumby_saver_canvas(s), ...);
 *           chumby_saver_damage(s, x, y, 32, 32);
 *   }
 *   ...
 *   s = chumby_saver_open(&(struct chumby_saver_opts){ .tick_ms = 80 });
 *   return chumby_saver_run(s, frame, NULL);
 *
 * The canvas starts as the backdrop: with a GPU, see-through (transparent,
 * premultiplied ARGB: the compositor blurs what's under it); without one,
 * the screen as the saver came up, blurred and dimmed once (opaque, so
 * the compositor draws nothing under it) - or black, if asked for.
 */
#ifndef CHUMBY_SAVER_H
#define CHUMBY_SAVER_H

#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>

struct chumby_saver;

struct chumby_saver_opts {
	int tick_ms;		/* a frame this often (default 80: 12.5 fps) */
	bool black;		/* no GPU: black, not the blurred screen */
};

/* Is there a GPU (the canvas see-through)? For a saver's choices (more
 * frames, say) before it opens. */
bool chumby_saver_gpu(void);

/* The window up, the backdrop made; NULL (and a message) if it can't be. */
struct chumby_saver *chumby_saver_open(const struct chumby_saver_opts *opts);

int chumby_saver_width(const struct chumby_saver *s);
int chumby_saver_height(const struct chumby_saver *s);
bool chumby_saver_see_through(const struct chumby_saver *s);

/* Where to draw: a8r8g8b8 premultiplied when see-through, else x8r8g8b8. */
pixman_image_t *chumby_saver_canvas(struct chumby_saver *s);

/* That much of the canvas changed (clipped to it; any number a frame). */
void chumby_saver_damage(struct chumby_saver *s, int x, int y, int w, int h);

/* The backdrop back over a rectangle (clipped), and damaged: what a moving
 * sprite leaves behind. */
void chumby_saver_erase(struct chumby_saver *s, int x, int y, int w, int h);

/* Every tick_ms (now_ms: monotonic), until it ends: then 0 (nonzero if
 * the compositor went away). */
typedef void (*chumby_saver_frame_cb)(struct chumby_saver *s, uint64_t now_ms, void *user);
int chumby_saver_run(struct chumby_saver *s, chumby_saver_frame_cb frame, void *user);

#endif
