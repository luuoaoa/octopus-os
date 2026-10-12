// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * octopus-airplay-meta: what AirPlay is playing, for the AirPlay app.
 *
 *   octopus-airplay-meta [metadata pipe]   (/run/shairport-sync/metadata)
 *
 * shairport-sync writes its metadata into a pipe as XML-ish items:
 *
 *   <item><type>636f7265</type><code>6d696e6d</code><length>5</length>
 *   <data encoding="base64">
 *   SGVsbG8=</data></item>
 *
 * type and code are four characters in hex ("core"/"minm": the title),
 * the data base64. We keep what matters in $run_dir/airplay (written whole,
 * renamed into place): "state=" playing / paused / ended / stopped,
 * "title=",
 * "artist=", "album=", "client=" (the sending device's name), "cover="
 * (the cover art, a file next to the pipe), "cover_id=" (changes with it),
 * and for remote control of the sender (octopus-airplay-remote) its
 * "dacp_id=", "active_remote=" (token) and "client_ip=".
 * "ended" is the phone ending the session (it does, a few seconds after
 * it pauses): what played and who sent it stay, so the app can offer to
 * play on (the phone takes "play" and comes back). "stopped" is nothing:
 * at the start, which is also whenever shairport-sync restarts (our
 * service is part of it), as when another player takes over.
 * Started with shairport-sync (octopus-airplay-meta.service); it reads the
 * pipe whether the app runs or not, so the app sees the current track
 * even when opened late.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PIPE_DEFAULT	"/run/shairport-sync/metadata"
#define COVER_BASE	"/run/shairport-sync/cover"
#define RUN_DEFAULT	"/run/octopus"

static char state[16] = "stopped", title[256], artist[256], album[256], client[128];
static char cover[64], dacp_id[64], active_remote[64], client_ip[80];
static unsigned cover_id;
static const char *run_dir;

static void state_write(void)
{
	char path[256], tmp[260];
	FILE *f;

	mkdir(run_dir, 0755);
	snprintf(path, sizeof(path), "%s/airplay", run_dir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "state=%s\ntitle=%s\nartist=%s\nalbum=%s\nclient=%s\ncover=%s\ncover_id=%u\n"
		"dacp_id=%s\nactive_remote=%s\nclient_ip=%s\n",
		state, title, artist, album, client, cover, cover_id,
		dacp_id, active_remote, client_ip);
	fclose(f);
	rename(tmp, path);
}

static int b64(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

/* In place; returns the decoded length. */
static size_t b64_decode(char *s, size_t n)
{
	size_t o = 0, i;
	unsigned acc = 0;
	int bits = 0, v;

	for (i = 0; i < n; i++) {
		v = b64((unsigned char)s[i]);
		if (v < 0)
			continue;	/* newlines, padding */
		acc = acc << 6 | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			s[o++] = (char)(acc >> bits & 0xff);
		}
	}
	return o;
}

static unsigned hex4(const char *s)
{
	return (unsigned)strtoul(s, NULL, 16);
}

#define CODE(a, b, c, d) ((unsigned)(a) << 24 | (unsigned)(b) << 16 | (unsigned)(c) << 8 | (unsigned)(d))

static void text_set(char *dst, size_t size, const char *d, size_t n)
{
	size_t i;

	if (n >= size)
		n = size - 1;
	for (i = 0; i < n; i++)		/* one line each */
		dst[i] = d[i] == '\n' || d[i] == '\r' ? ' ' : d[i];
	dst[n] = '\0';
}

static void cover_save(const char *d, size_t n)
{
	const char *ext = NULL;
	char path[96], tmp[100];
	FILE *f;

	if (n > 3 && (unsigned char)d[0] == 0xff && (unsigned char)d[1] == 0xd8)
		ext = ".jpg";
	else if (n > 4 && !memcmp(d, "\x89PNG", 4))
		ext = ".png";
	if (!ext) {		/* none (an empty PICT: no art) */
		cover[0] = '\0';
		return;
	}
	snprintf(path, sizeof(path), COVER_BASE "%s", ext);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "wb");
	if (!f)
		return;
	fwrite(d, 1, n, f);
	fclose(f);
	rename(tmp, path);
	snprintf(cover, sizeof(cover), "%s", path);
	cover_id++;
}

static void item(unsigned type, unsigned code, char *data, size_t n)
{
	if (type == CODE('c', 'o', 'r', 'e')) {
		if (code == CODE('m', 'i', 'n', 'm'))
			text_set(title, sizeof(title), data, n);
		else if (code == CODE('a', 's', 'a', 'r'))
			text_set(artist, sizeof(artist), data, n);
		else if (code == CODE('a', 's', 'a', 'l'))
			text_set(album, sizeof(album), data, n);
		else
			return;
	} else if (type == CODE('s', 's', 'n', 'c')) {
		if (code == CODE('P', 'I', 'C', 'T')) {
			cover_save(data, n);
		} else if (code == CODE('s', 'n', 'a', 'm')) {
			text_set(client, sizeof(client), data, n);
		} else if (code == CODE('d', 'a', 'i', 'd')) {
			text_set(dacp_id, sizeof(dacp_id), data, n);
		} else if (code == CODE('a', 'c', 'r', 'e')) {
			text_set(active_remote, sizeof(active_remote), data, n);
		} else if (code == CODE('c', 'l', 'i', 'p')) {
			text_set(client_ip, sizeof(client_ip), data, n);
		} else if (code == CODE('p', 'b', 'e', 'g') || code == CODE('p', 'r', 's', 'm')) {
			snprintf(state, sizeof(state), "playing");
		} else if (code == CODE('p', 'f', 'l', 's') || code == CODE('p', 'p', 'a', 'u')) {
			snprintf(state, sizeof(state), "paused");
		} else if (code == CODE('p', 'e', 'n', 'd')) {
			snprintf(state, sizeof(state), "ended");
		} else {
			return;
		}
	} else {
		return;
	}
	state_write();
}

/* The text between <tag> and </tag> in s, or NULL. */
static char *between(char *s, const char *open, const char *close, char **after)
{
	char *a = strstr(s, open), *b;

	if (!a)
		return NULL;
	a += strlen(open);
	b = strstr(a, close);
	if (!b)
		return NULL;
	*b = '\0';
	if (after)
		*after = b + strlen(close);
	return a;
}

int main(int argc, char **argv)
{
	const char *pipe = argc > 1 ? argv[1] : PIPE_DEFAULT;
	size_t cap = 1 << 16, len = 0;
	char *buf = malloc(cap + 1);
	int fd;

	run_dir = getenv("OCTOPUS_RUN_DIR") && *getenv("OCTOPUS_RUN_DIR") ?
		  getenv("OCTOPUS_RUN_DIR") : RUN_DEFAULT;
	if (!buf)
		return 1;
	/* Read-write: never an end of file between writers. */
	while ((fd = open(pipe, O_RDWR)) < 0) {
		if (errno == ENOENT)
			mkfifo(pipe, 0644);
		sleep(1);
	}
	state_write();
	for (;;) {
		ssize_t n;
		char *start, *end;

		if (len == cap) {	/* a big cover: room for it (up to 16 MB) */
			char *nb = cap < (16 << 20) ? realloc(buf, cap * 2 + 1) : NULL;

			if (!nb) {
				len = 0;	/* hopeless: start over */
				continue;
			}
			buf = nb;
			cap *= 2;
		}
		n = read(fd, buf + len, cap - len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			sleep(1);
			continue;
		}
		len += n;
		buf[len] = '\0';
		/* Every whole item in the buffer. */
		while ((start = strstr(buf, "<item>")) && (end = strstr(start, "</item>"))) {
			char *t, *c, *d, *rest;
			unsigned type = 0, code = 0;
			size_t dn = 0;

			end += strlen("</item>");
			*(end - strlen("</item>")) = '\0';
			t = between(start, "<type>", "</type>", &rest);
			if (t) {
				type = hex4(t);
				c = between(rest, "<code>", "</code>", &rest);
				if (c) {
					code = hex4(c);
					d = between(rest, "<data encoding=\"base64\">", "</data>", NULL);
					if (d)
						dn = b64_decode(d, strlen(d));
					item(type, code, d ? d : (char *)"", dn);
				}
			}
			len -= end - buf;
			memmove(buf, end, len);
			buf[len] = '\0';
		}
		/* Junk before an item (not one of ours): drop it. */
		if (!strstr(buf, "<item>") && len > 4096)
			len = 0;
	}
}
