/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RDS decoder for the qn8005 tool: station name (PS, groups 0A/0B),
 * RadioText (2A/2B) and RadioText+ title/artist tags (ODA 0x4BD7,
 * announced in 3A). Fed one group of four blocks at a time.
 *
 * The QN8005 gives no per-block error flags, so every 2- or 4-character
 * segment is only taken after the same value arrived twice in a row.
 */
#ifndef RDS_H
#define RDS_H

#include <stdbool.h>
#include <stdint.h>

#define RDS_RT_MAX	64

struct rds {
	uint16_t pi;
	int pty;
	char ps[9];			/* published: complete names only */
	char rt[RDS_RT_MAX + 1];	/* published: complete texts only */
	char title[RDS_RT_MAX + 1];	/* from RadioText+ */
	char artist[RDS_RT_MAX + 1];
	bool changed;			/* set when anything published changed */

	/* working state */
	uint16_t pi_cand;		/* a new PI, until seen twice */
	char ps_work[8];
	uint8_t ps_last[4][2], ps_seen[4], ps_ok;
	char rt_work[RDS_RT_MAX];
	uint8_t rt_last[16][4], rt_seen[16];
	uint16_t rt_ok;
	int rt_ab, rt_len, rt_b;	/* A/B flag, end marker position, 2B? */
	int rtp_group;			/* RT+ group type code, -1 = none */
};

void rds_init(struct rds *r);
void rds_group(struct rds *r, const uint16_t blk[4]);

#endif
