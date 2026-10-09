// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * qn8005 - control the Chumby One's Quintic QN8005B FM tuner from userland
 * over a standard i2c-dev bus.
 *
 * The register sequences replicate Chumby's stock /bin/fmradio
 * (fmradio-qn8005, release 1.0.3454), reverse-engineered from the binary:
 * Quintic's reference driver (QND_* / QNF_*) on top of the vendor kernel's
 * quirky 8-bit-address I2C driver. With a mainline i2c-mxs the tuner is a
 * plain 7-bit device at 0x2b.
 *
 * Like the stock tool, each invocation is stateless: "init" programs the
 * chip, later "tune" calls run with the per-band noise calibration zeroed,
 * which is exactly how the stock firmware's separate fmradio calls behaved.
 *
 * Usage: qn8005 [-d /dev/i2c-N] [-a addr] <command> [args]
 *   init [--xclk]     power up and initialise (receive mode, muted -> unmuted)
 *   tune <MHz>        tune, e.g. "tune 100.3"
 *   mute | unmute
 *   mono | stereo
 *   rssi              print the RSSI of the current channel
 *   freq              print the current frequency
 *   scan [us|eu|jp]   software scan of the band, print channels above threshold
 *   rds <file>        keep decoding RDS (station name, RadioText, RT+
 *                     title/artist) into <file> until killed
 *   r <reg> | w <reg> <val> | dump
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rds.h"

static int fd = -1;
static int addr = 0x2b;

static void msleep(unsigned int ms)
{
	usleep(ms * 1000);
}

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };

	if (write(fd, buf, 2) != 2) {
		fprintf(stderr, "qn8005: write reg 0x%02x: %s\n", reg, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * The QN8005's register read is not a standard write-then-read: it wants
 * START, the address with the *read* bit, the register number written,
 * then the data clocked out - one transaction, no repeated start (what
 * the vendor kernel's "CHUMBY_radio_fix" generated). A repeated-start
 * read is NAKed. In Linux terms: a write with I2C_M_REV_DIR_ADDR, then a
 * read with I2C_M_NOSTART - protocol mangling, which the bit-banged
 * i2c-gpio bus (see the DTS) supports.
 */
static int rd(uint8_t reg)
{
	uint8_t val;
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = I2C_M_REV_DIR_ADDR, .len = 1, .buf = &reg },
		{ .addr = addr, .flags = I2C_M_RD | I2C_M_NOSTART, .len = 1, .buf = &val },
	};
	struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };

	if (ioctl(fd, I2C_RDWR, &data) < 0) {
		fprintf(stderr, "qn8005: read reg 0x%02x: %s\n", reg, strerror(errno));
		return -1;
	}
	return val;
}

/* QNF_SetRegBit: clear `mask`, OR in `val` (val is not masked: matches stock). */
static int setbits(uint8_t reg, uint8_t mask, uint8_t val)
{
	int v = rd(reg);

	if (v < 0)
		return -1;
	return wr(reg, (v & ~mask) | val);
}

/* Channel <-> frequency in 10 kHz units: f = 7600 + 5 * ch (50 kHz grid). */
static int set_ch(unsigned int f10k)
{
	unsigned int ch = (f10k - 7600) / 5;
	int r11;

	if (wr(0x08, ch & 0xff))
		return -1;
	r11 = rd(0x0b);
	if (r11 < 0)
		return -1;
	return wr(0x0b, (r11 & ~0x03) | ((ch >> 8) & 0x03));
}

static int get_ch(void)
{
	int hi = rd(0x0b), lo = rd(0x08);

	if (hi < 0 || lo < 0)
		return -1;
	return 7600 + 5 * (((hi & 3) << 8) | lo);
}

/* QND_SetSysMode(QND_MODE_RX | QND_MODE_FM) */
static int set_rx_mode(void)
{
	return setbits(0x00, 0xd0, 0x80);
}

static int get_rssi_at(unsigned int f10k)
{
	if (set_rx_mode() || set_ch(f10k) || setbits(0x00, 0x20, 0x01))
		return -1;
	msleep(10);
	return rd(0x1c);
}

/* QNF_GetFMRssiAvg: noise-floor calibration run by QND_Init. */
static int rssi_calibrate(void)
{
	unsigned int band, i;
	int floor = 255;

	for (band = 0; band < 4; band++) {
		int bmin = 255;

		for (i = 0; i < 4; i++) {
			int r = get_rssi_at(7630 + band * 800 + i * 10);

			if (r < 0)
				return -1;
			if (r < bmin)
				bmin = r;
		}
		if (bmin < floor)
			floor = bmin;
	}

	if (setbits(0x40, 0x3f, floor > 31 ? 0x3f : (floor + 31) & 0xff) ||
	    setbits(0x41, 0x7f, floor > 77 ? 0x7f : (floor + 50) & 0xff) ||
	    setbits(0x42, 0xbf, floor > 44 ? 0xbf : 0x80 | ((floor + 19) & 0x7f)))
		return -1;
	return 0;
}

static int write_seq(const uint8_t (*seq)[2], size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (wr(seq[i][0], seq[i][1]))
			return -1;
	return 0;
}

/*
 * Stock fmradio: if /psp/fmradio_xclk exists, mux PWM3 (bank1 pin 29) out
 * and run it in "MATT" pass-through mode as the tuner's reference clock.
 */
static int enable_xclk(void)
{
	const off_t pinctrl = 0x80018000, pwm = 0x80064000;
	volatile uint32_t *p, *q;
	int mfd = open("/dev/mem", O_RDWR | O_SYNC);

	if (mfd < 0) {
		perror("qn8005: /dev/mem");
		return -1;
	}
	p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, pinctrl);
	q = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, pwm);
	close(mfd);
	if (p == MAP_FAILED || q == MAP_FAILED) {
		perror("qn8005: mmap");
		return -1;
	}
	p[0x138 / 4] = 0x0c000000;	/* HW_PINCTRL_MUXSEL3_CLR: PWM3 function */
	q[0x080 / 4] = 0x01800000;	/* HW_PWM_PERIOD3: MATT | MATT_SEL */
	q[0x004 / 4] = 0x00000008;	/* HW_PWM_CTRL_SET: PWM3_ENABLE */
	return 0;
}

/* QN_ChipInitialization + QND_Init + SetCountry/SetSysMode(RX) */
static int chip_init(int xclk)
{
	static const uint8_t pre[][2] = {
		{ 0x01, 0x89 }, { 0x03, 0x29 },
	};
	static const uint8_t mid[][2] = {
		{ 0x50, 0x00 }, { 0x01, 0x49 }, { 0x01, 0x09 },
	};
	static const uint8_t post[][2] = {
		{ 0x3c, 0x89 }, { 0x4a, 0xba }, { 0x5c, 0x05 }, { 0x52, 0x0c },
		{ 0x00, 0x81 }, { 0x57, 0x80 }, { 0x57, 0x00 },
	};

	if (xclk && enable_xclk())
		return -1;

	msleep(200);
	if (write_seq(pre, 2))
		return -1;
	if (wr(0x49, xclk ? 0x10 : 0x00) || wr(0x04, xclk ? 0x3f : 0x00))
		return -1;
	if (write_seq(mid, 3))
		return -1;
	msleep(400);
	if (write_seq(post, 7))
		return -1;
	msleep(100);
	if (wr(0x00, 0x01) || wr(0x52, 0x00))
		return -1;

	/* QND_Init */
	if (wr(0x52, 0x0c) || wr(0x3f, 0xff) || wr(0x43, 0x12) ||
	    setbits(0x1f, 0x38, 0x28) || wr(0x52, 0x00) || wr(0x00, 0x01))
		return -1;
	msleep(1500);
	if (wr(0x52, 0x0c) || rssi_calibrate() || wr(0x52, 0x00) ||
	    wr(0x00, 0x01))
		return -1;

	/* QND_SetSysMode(QND_MODE_RX | QND_MODE_FM) */
	return set_rx_mode();
}

/* QND_TuneToCH, with the stock tool's (zeroed) per-band calibration. */
static int tune(unsigned int f10k)
{
	int rssi;

	/* Image/spur avoidance every 2.4 MHz from 77.10 MHz */
	if (setbits(0x0d, 0x20, ((f10k - 7710) % 240) == 0 ? 0x20 : 0x00))
		return -1;
	if (set_ch(f10k) || setbits(0x00, 0x20, 0x01))
		return -1;

	rssi = rd(0x00);
	if (rssi < 0)
		return -1;
	if (rssi & 0x80) {
		/* Rssinarray[band] == 0 for a stateless invocation */
		if (wr(0x40, 12) || wr(0x42, 12) || wr(0x41, 18))
			return -1;
		msleep(100);
		rssi = rd(0x1c);
		if (rssi < 0)
			return -1;
		/* RSSIn is still its initial 255: always the weak-signal branch */
		if (setbits(0x41, 0x80, 0x80) || wr(0x38, 0x1f) || wr(0x3e, 0x48))
			return -1;
	}

	/* fmradio "t": unmute after tuning */
	return wr(0x52, 0x00);
}

static int parse_mhz(const char *s)
{
	double mhz = strtod(s, NULL);
	int f = (int)(mhz * 100 + 0.5);

	if (f < 7600 || f > 10800) {
		fprintf(stderr, "qn8005: frequency %s out of range (76-108 MHz)\n", s);
		return -1;
	}
	return f - (f % 5);	/* 50 kHz grid */
}

#define MAX_CHANNELS	256	/* 76-90 MHz or 87.5-108 MHz in 0.1 steps */

static int scan(const char *region)
{
	unsigned int start = 8790, stop = 10790, step = 20;	/* stock "US" */
	int rssi[MAX_CHANNELS];
	unsigned int f;
	int threshold, i, n;

	if (region && !strcmp(region, "eu")) {
		start = 8750; stop = 10800; step = 10;
	} else if (region && !strcmp(region, "jp")) {
		start = 7600; stop = 9000; step = 10;
	}

	if (wr(0x52, 0x0c))
		return -1;
	/* Threshold: noise floor (min RSSI over a few channels) + margin */
	threshold = 255;
	for (f = start; f <= stop; f += (stop - start) / 8) {
		int r = get_rssi_at(f);

		if (r < 0)
			return -1;
		if (r < threshold)
			threshold = r;
	}
	printf("noise floor %d\n", threshold);	/* for relative levels */
	threshold += 12;

	/* Measure the band, then report only peaks above the threshold: a
	 * strong station also lifts its neighbouring channels. */
	for (f = start, n = 0; f <= stop && n < MAX_CHANNELS; f += step)
		if ((rssi[n++] = get_rssi_at(f)) < 0)
			return -1;
	for (i = 0; i < n; i++) {
		f = start + i * step;
		if (rssi[i] >= threshold &&
		    (i == 0 || rssi[i] >= rssi[i - 1]) &&
		    (i == n - 1 || rssi[i] > rssi[i + 1]))
			printf("%u.%02u MHz  rssi %d\n", f / 100, f % 100, rssi[i]);
	}
	return wr(0x52, 0x00);
}

/*
 * RDS (stock QND_RDSEnable / QND_RDSCheckBufferReady / QND_RDSLoadData):
 * reg 0x00 bit 1 enables it; bit 7 of reg 0x1b flips each time a group
 * is ready in regs 0x10..0x17 (blocks A..D, high byte first), and its
 * bits 3..0 flag blocks A..D received with errors. Groups come
 * every ~88 ms; poll at 20 ms. Decoded state goes to `out` as key=value
 * lines (ps, rt, title, artist, pty, pi, psdyn = 1 while the PS scrolls
 * text, pstext = those words joined, and the signal strength rssi,
 * sampled about once a second), rewritten atomically on change.
 */
static volatile sig_atomic_t rds_quit;
static int rds_rssi = -1;	/* signal strength, also published (rssi=) */

static void rds_signal(int sig)
{
	(void)sig;
	rds_quit = 1;
}

static void rds_write(const char *out, const struct rds *r)
{
	char tmp[512];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", out);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "pi=%04x\npty=%d\nps=%s\nrt=%s\ntitle=%s\nartist=%s\npsdyn=%d\n"
		"pstext=%s\nrssi=%d\n", r->pi, r->pty, r->ps, r->rt, r->title, r->artist,
		r->ps_dynamic, r->pstext, rds_rssi);
	fclose(f);
	rename(tmp, out);
}

static int rds_run(const char *out)
{
	char dir[256], *slash;
	struct rds r;
	int prev, v, i, tick = 0;

	/* The output usually lives in /run/chumby, which may not exist yet. */
	snprintf(dir, sizeof(dir), "%s", out);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	signal(SIGTERM, rds_signal);
	signal(SIGINT, rds_signal);
	rds_init(&r);
	rds_write(out, &r);
	if (setbits(0x00, 0x02, 0x02) < 0 || (prev = rd(0x1b)) < 0)
		return -1;
	prev &= 0x80;

	while (!rds_quit) {
		uint8_t raw[8];
		uint16_t blk[4];

		msleep(20);
		if (++tick >= 50) {	/* ~1 s */
			tick = 0;
			v = rd(0x1c);
			if (v >= 0 && v != rds_rssi) {
				rds_rssi = v;
				rds_write(out, &r);
			}
		}
		v = rd(0x1b);
		if (v < 0 || (v & 0x80) == prev)
			continue;
		prev = v & 0x80;
		for (i = 0; i < 8; i++) {
			int b = rd(0x10 + i);

			if (b < 0)
				break;
			raw[i] = b;
		}
		if (i < 8)
			continue;
		for (i = 0; i < 4; i++)
			blk[i] = raw[2 * i] << 8 | raw[2 * i + 1];
		rds_group(&r, blk, v & 0x0f);	/* the bad-block flags */
		if (r.changed) {
			r.changed = false;
			rds_write(out, &r);
		}
	}
	return 0;
}

/* Reception check: each group's status (reg 0x1b) and blocks for a few
 * seconds, and how many came with all four blocks clean (low nibble 0:
 * the per-block error flags). */
static int rds_debug(int secs)
{
	int prev, v, i, groups = 0, clean = 0, ms;

	if (setbits(0x00, 0x02, 0x02) < 0 || (prev = rd(0x1b)) < 0)
		return -1;
	prev &= 0x80;
	for (ms = 0; ms < secs * 1000; ms += 10) {
		uint8_t raw[8];

		msleep(10);
		v = rd(0x1b);
		if (v < 0 || (v & 0x80) == prev)
			continue;
		prev = v & 0x80;
		for (i = 0; i < 8; i++)
			raw[i] = rd(0x10 + i);
		groups++;
		if (!(v & 0x0f))
			clean++;
		printf("%02x  %02x%02x %02x%02x %02x%02x %02x%02x\n", v, raw[0], raw[1],
		       raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
	}
	printf("%d groups in %d s, %d clean, rssi %d\n", groups, secs, clean, rd(0x1c));
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: qn8005 [-d /dev/i2c-N] [-a addr] <command>\n"
		"  init [--xclk] | tune <MHz> | mute | unmute | mono | stereo\n"
		"  rssi | freq | scan [us|eu|jp] | rds <file> | rdsdebug [secs]\n"
		"  r <reg> | w <reg> <val> | dump\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/i2c-0";
	const char *cmd;
	int opt, ret = 0;

	/* "+": stop at the command, so its own options (init --xclk) stay. */
	while ((opt = getopt(argc, argv, "+d:a:h")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'a': addr = strtol(optarg, NULL, 0); break;
		default: usage();
		}
	}
	if (optind >= argc)
		usage();
	cmd = argv[optind++];

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "qn8005: %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (ioctl(fd, I2C_SLAVE, addr) < 0) {
		fprintf(stderr, "qn8005: I2C_SLAVE 0x%02x: %s\n", addr, strerror(errno));
		return 1;
	}

	if (!strcmp(cmd, "init")) {
		int xclk = optind < argc && !strcmp(argv[optind], "--xclk");

		ret = chip_init(xclk);
	} else if (!strcmp(cmd, "tune")) {
		int f;

		if (optind >= argc)
			usage();
		f = parse_mhz(argv[optind]);
		ret = f < 0 ? -1 : tune(f);
	} else if (!strcmp(cmd, "mute") || !strcmp(cmd, "unmute")) {
		ret = wr(0x52, cmd[0] == 'm' ? 0x0c : 0x00);
	} else if (!strcmp(cmd, "mono") || !strcmp(cmd, "stereo")) {
		/* QND_RXConfigAudio(QND_CONFIG_MONO, on/off) -> QNF_SetAudioMono */
		int mono = !strcmp(cmd, "mono");

		ret = wr(0x0e, mono ? 0x20 : 0x6c) ||
		      setbits(0x01, 0x20, mono ? 0x20 : 0x00);
	} else if (!strcmp(cmd, "rssi")) {
		int r = rd(0x1c);

		if (r < 0)
			ret = -1;
		else
			printf("%d\n", r);
	} else if (!strcmp(cmd, "freq")) {
		int f = get_ch();

		if (f < 0)
			ret = -1;
		else
			printf("%d.%02d\n", f / 100, f % 100);
	} else if (!strcmp(cmd, "rds")) {
		if (optind >= argc)
			usage();
		ret = rds_run(argv[optind]);
	} else if (!strcmp(cmd, "rdsdebug")) {
		ret = rds_debug(optind < argc ? atoi(argv[optind]) : 10);
	} else if (!strcmp(cmd, "scan")) {
		ret = scan(optind < argc ? argv[optind] : NULL);
	} else if (!strcmp(cmd, "r")) {
		int v;

		if (optind >= argc)
			usage();
		v = rd(strtol(argv[optind], NULL, 0));
		if (v < 0)
			ret = -1;
		else
			printf("0x%02x\n", v);
	} else if (!strcmp(cmd, "w")) {
		if (optind + 1 >= argc)
			usage();
		ret = wr(strtol(argv[optind], NULL, 0),
			 strtol(argv[optind + 1], NULL, 0));
	} else if (!strcmp(cmd, "dump")) {
		int reg;

		for (reg = 0; reg < 0x60; reg++) {
			int v = rd(reg);

			if (v < 0) {
				ret = -1;
				break;
			}
			printf("%02x%s", v, (reg & 15) == 15 ? "\n" : " ");
		}
	} else {
		usage();
	}

	close(fd);
	return ret ? 1 : 0;
}
