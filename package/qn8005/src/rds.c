// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RDS decoder: PS, RadioText, RadioText+. See rds.h.
 */

#include <string.h>

#include "rds.h"

#define RTPLUS_AID	0x4bd7
#define RTP_TITLE	1	/* ITEM.TITLE */
#define RTP_ARTIST	4	/* ITEM.ARTIST */

void rds_init(struct rds *r)
{
	memset(r, 0, sizeof(*r));
	r->pty = -1;
	r->rt_ab = -1;
	r->rtp_group = -1;
	memset(r->ps_work, ' ', sizeof(r->ps_work));
	memset(r->rt_work, ' ', sizeof(r->rt_work));
}

/* RDS uses its own character table; keep printable ASCII, the rest '?'. */
static char rds_char(uint8_t c)
{
	return c >= 0x20 && c < 0x7f ? (char)c : '?';
}

static void set_str(struct rds *r, char *dst, const char *src, size_t n)
{
	char buf[RDS_RT_MAX + 1];

	/* Trim spaces at both ends. */
	while (n && *src == ' ') {
		src++;
		n--;
	}
	while (n && src[n - 1] == ' ')
		n--;
	memcpy(buf, src, n);
	buf[n] = '\0';
	if (strcmp(dst, buf)) {
		strcpy(dst, buf);
		r->changed = true;
	}
}

/* Accept a segment only when the same bytes arrive twice in a row. */
static bool confirmed(uint8_t *last, uint8_t *seen, const uint8_t *now, int n)
{
	if (*seen && !memcmp(last, now, n))
		return true;
	memcpy(last, now, n);
	*seen = 1;
	return false;
}

static void do_ps(struct rds *r, uint16_t b, uint16_t d)
{
	int seg = b & 3;
	uint8_t now[2] = { d >> 8, d & 0xff };

	if (!confirmed(r->ps_last[seg], &r->ps_seen[seg], now, 2))
		return;
	r->ps_work[seg * 2] = rds_char(now[0]);
	r->ps_work[seg * 2 + 1] = rds_char(now[1]);
	r->ps_ok |= 1 << seg;
	/* Publish whole names: also follows stations that scroll their PS. */
	if (r->ps_ok == 0xf) {
		set_str(r, r->ps, r->ps_work, 8);
		r->ps_ok = 0;
	}
}

static void rt_reset(struct rds *r)
{
	memset(r->rt_work, ' ', sizeof(r->rt_work));
	memset(r->rt_seen, 0, sizeof(r->rt_seen));
	r->rt_ok = 0;
	r->rt_len = RDS_RT_MAX;
}

static void do_rt(struct rds *r, uint16_t b, uint16_t c, uint16_t d, bool version_b)
{
	int seg = b & 0xf, ab = !!(b & 0x10), per = version_b ? 2 : 4, i, need;
	uint8_t now[4] = { c >> 8, c & 0xff, d >> 8, d & 0xff };
	const uint8_t *chars = version_b ? now + 2 : now;

	/* A/B flips when the station starts a new text. */
	if (ab != r->rt_ab || version_b != r->rt_b) {
		r->rt_ab = ab;
		r->rt_b = version_b;
		rt_reset(r);
	}
	if (!confirmed(r->rt_last[seg], &r->rt_seen[seg], chars, per))
		return;
	for (i = 0; i < per; i++) {
		int pos = seg * per + i;

		if (chars[i] == 0x0d) {		/* end of text */
			if (pos < r->rt_len)
				r->rt_len = pos;
			break;
		}
		if (pos < RDS_RT_MAX)
			r->rt_work[pos] = rds_char(chars[i]);
	}
	r->rt_ok |= 1 << seg;
	if (version_b && r->rt_len > 32)
		r->rt_len = 32;
	/* Complete once every segment up to the end has been confirmed. */
	need = (r->rt_len + per - 1) / per;
	if ((r->rt_ok & ((1u << need) - 1)) == (1u << need) - 1)
		set_str(r, r->rt, r->rt_work, r->rt_len);
}

static void rtp_tag(struct rds *r, int type, int start, int len)
{
	size_t n = strlen(r->rt);

	if (start + len > (int)n)
		return;		/* tag for a text we don't have yet */
	if (type == RTP_TITLE)
		set_str(r, r->title, r->rt + start, len);
	else if (type == RTP_ARTIST)
		set_str(r, r->artist, r->rt + start, len);
}

/* RadioText+ (IEC 62106 annex / RT+ spec): two tags per group. */
static void do_rtplus(struct rds *r, uint16_t b, uint16_t c, uint16_t d)
{
	bool running = b & 0x08;

	if (!running) {
		set_str(r, r->title, "", 0);
		set_str(r, r->artist, "", 0);
		return;
	}
	rtp_tag(r, ((b & 7) << 3) | (c >> 13), (c >> 7) & 0x3f, ((c >> 1) & 0x3f) + 1);
	rtp_tag(r, ((c & 1) << 5) | (d >> 11), (d >> 5) & 0x3f, (d & 0x1f) + 1);
}

void rds_group(struct rds *r, const uint16_t blk[4])
{
	uint16_t a = blk[0], b = blk[1], c = blk[2], d = blk[3];
	int type = b >> 12, code = (type << 1) | !!(b & 0x800);
	bool version_b = b & 0x800;
	int pty = (b >> 5) & 0x1f;

	/* A new station (PI) counts once seen twice in a row: a corrupted
	 * block A must not wipe what we have. Until then, drop the group. */
	if (a != r->pi) {
		if (a != r->pi_cand) {
			r->pi_cand = a;
			return;
		}
		if (r->pi) {
			bool had = r->ps[0] || r->rt[0];

			rds_init(r);
			r->changed = had;
		}
		r->pi = a;
	}
	if (pty != r->pty) {
		r->pty = pty;
		r->changed = true;
	}
	if (code == r->rtp_group && r->rtp_group >= 0)
		do_rtplus(r, b, c, d);
	switch (type) {
	case 0:
		do_ps(r, b, d);
		break;
	case 2:
		do_rt(r, b, c, d, version_b);
		break;
	case 3:
		/* ODA announcement: which group carries RT+. */
		if (!version_b && d == RTPLUS_AID)
			r->rtp_group = b & 0x1f;
		break;
	}
}
