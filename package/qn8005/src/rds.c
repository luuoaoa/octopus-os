// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RDS decoder: PS, RadioText, RadioText+. See rds.h.
 */

#include <string.h>

#include "rds.h"

#define RTPLUS_AID	0x4bd7
#define RTP_TITLE	1	/* ITEM.TITLE */
#define RTP_ARTIST	4	/* ITEM.ARTIST */
#define PI_CONFIRM	4	/* groups in a row before a new PI counts */
#define PS_DYN_GROUPS	342	/* ~30 s: 3 PS changes within it = scrolling */
#define PS_STILL_GROUPS	1368	/* ~2 min without a change: a name again */
#define PS_CYCLE_WORDS	24	/* a cycle longer than this: publish anyway */

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
	char buf[RDS_PSTEXT_MAX + 1];	/* the longest field */

	/* Trim spaces at both ends. */
	while (n && *src == ' ') {
		src++;
		n--;
	}
	while (n && src[n - 1] == ' ')
		n--;
	if (n > RDS_PSTEXT_MAX)
		n = RDS_PSTEXT_MAX;
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

/* The trimmed 8 characters of a PS. */
static void ps_trim(const char *ps8, char out[9])
{
	int s = 0, e = 8;

	while (s < e && ps8[s] == ' ')
		s++;
	while (e > s && ps8[e - 1] == ' ')
		e--;
	memcpy(out, ps8 + s, e - s);
	out[e - s] = '\0';
}

/* A new (whole) PS: note how often it changes; while it scrolls, collect
 * its words, publishing a cycle when it starts over (its first word
 * again) or runs long. */
static void ps_scroll(struct rds *r, const char *word)
{
	int i, recent = 0;

	memmove(r->ps_change + 1, r->ps_change, sizeof(r->ps_change) - sizeof(r->ps_change[0]));
	r->ps_change[0] = r->groups;
	for (i = 0; i < 3; i++)
		if (r->ps_change[i] && r->groups - r->ps_change[i] <= PS_DYN_GROUPS)
			recent++;
	/* Sticky: missed words make it change less often for a while. */
	if (recent == 3 && !r->ps_dynamic) {
		r->ps_dynamic = true;
		r->changed = true;
	}
	if (!*word)
		return;
	if ((r->ps_words >= 2 && !strcmp(word, r->ps_first)) || r->ps_words >= PS_CYCLE_WORDS) {
		set_str(r, r->pstext, r->ps_cycle, strlen(r->ps_cycle));
		r->ps_cycle[0] = '\0';
		r->ps_words = 0;
	}
	if (!r->ps_words)
		strcpy(r->ps_first, word);
	if (strlen(r->ps_cycle) + strlen(word) + 2 < sizeof(r->ps_cycle)) {
		if (r->ps_cycle[0])
			strcat(r->ps_cycle, " ");
		strcat(r->ps_cycle, word);
	}
	r->ps_words++;
}

static void do_ps(struct rds *r, uint16_t b, uint16_t d)
{
	int seg = b & 3;
	uint8_t now[2] = { d >> 8, d & 0xff };
	char c0 = rds_char(now[0]), c1 = rds_char(now[1]), word[9];

	if (!confirmed(r->ps_last[seg], &r->ps_seen[seg], now, 2))
		return;
	/* A segment that changed starts a new name: never publish one put
	 * together from two (the station moved on mid-way: "Maexis"). */
	if (r->ps_work[seg * 2] != c0 || r->ps_work[seg * 2 + 1] != c1)
		r->ps_ok = 0;
	r->ps_work[seg * 2] = c0;
	r->ps_work[seg * 2 + 1] = c1;
	r->ps_ok |= 1 << seg;
	if (r->ps_ok != 0xf)
		return;
	r->ps_ok = 0;
	ps_trim(r->ps_work, word);
	if (strcmp(word, r->ps)) {
		set_str(r, r->ps, word, strlen(word));
		ps_scroll(r, word);
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

	r->groups++;
	/* A new station (PI) counts once seen PI_CONFIRM times in a row: a
	 * corrupted block A must not wipe what we have. Until then, drop the
	 * group. */
	if (a != r->pi) {
		if (!a)
			return;
		if (a != r->pi_cand) {
			r->pi_cand = a;
			r->pi_cand_n = 1;
			return;
		}
		if (++r->pi_cand_n < PI_CONFIRM)
			return;
		if (r->pi) {
			bool had = r->ps[0] || r->rt[0];
			unsigned long groups = r->groups;

			rds_init(r);
			r->groups = groups;
			r->changed = had;
		}
		r->pi = a;
	}
	if (pty != r->pty) {
		r->pty = pty;
		r->changed = true;
	}
	if (r->ps_dynamic && r->groups - r->ps_change[0] > PS_STILL_GROUPS) {
		r->ps_dynamic = false;	/* the PS stopped scrolling */
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
