// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-stream: play an internet radio stream.
 *
 *   chumby-stream <url> [name]   start playing (replacing any stream), in
 *                                the background; takes the sound from
 *                                other sources (chumby-audio play stream)
 *   chumby-stream stop
 *   chumby-stream pause | resume   for chumby-audio: stop for a while,
 *                                remembering what played; play it again
 *   chumby-stream active         exit status: playing?
 *
 * Over http or https (libcurl), to the codec (ALSA, as the FM loop uses
 * it):
 *   - Shoutcast/Icecast streams, MP3 (libmpg123) or AAC (faad2, fixed
 *     point: no FPU here); their "ICY" metadata gives what's playing.
 *     A thread downloads into a buffer of a few seconds, and playing
 *     starts (again, after running dry) once ~2 s are in: Wi-Fi hiccups
 *     don't reach the speaker
 *   - HLS (.m3u8): MPEG-TS or packed-audio segments of either, fetched
 *     by a thread on one kept-alive connection (a TLS handshake per 2 s
 *     segment would be too slow on this CPU) and queued for playing
 *   - .pls and .m3u playlists, followed to their first entry
 * A dropped stream is reconnected, backing off up to 30 s. We run above
 * the UI's priority, so redraws can't make the decoder late.
 *
 * State goes to $run_dir/stream for the app ("state=", "url=", "name=",
 * "title=", "codec=" MP3/AAC, "bitrate=" kbit/s as measured over a few
 * seconds) lines, gone once stopped; our pid to $run_dir/stream.pid.
 */

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <mpg123.h>
#include <neaacdec.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <chumby.h>

#define PCM		"plughw:0,0"
#define CA_BUNDLE	"/etc/ssl/certs/ca-certificates.crt"	/* mbed TLS: no dirs */
#define AUDIO		"/usr/bin/chumby-audio"
#define USER_AGENT	"chumby-stream/1.0"
#define BACKOFF_MAX_S	30

static volatile sig_atomic_t stop;
static char status_path[256], pid_path[256], paused_path[256];
static int bitrate;			/* kbit/s, measured */
static char url[1024], name[128], title[256], state[16];


static void sleep_s(int s);

static mpg123_handle *mh;
static NeAACDecHandle aac;
static bool aac_ready;
static unsigned char abuf[16384];	/* ADTS bytes not decoded yet */
static size_t alen;
static snd_pcm_t *pcm;
static long pcm_rate;
static int pcm_channels;

/* ICY: metaint bytes of audio, a length byte (x16), that much metadata. */
static long metaint, audio_left;
static int meta_len = -1, meta_got;
static char meta[255 * 16 + 1];
static bool got_name;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

enum codec { CODEC_UNKNOWN, CODEC_MP3, CODEC_AAC };
static enum codec codec;	/* of the stream: by Content-Type, else sniffed */

static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;

/* Called with status_lock held. */
static void status_write_locked(void)
{
	char tmp[300];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", status_path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "state=%s\nurl=%s\nname=%s\ntitle=%s\ncodec=%s\nbitrate=%d\n", state, url,
		name, title, codec == CODEC_AAC ? "AAC" : codec == CODEC_MP3 ? "MP3" : "", bitrate);
	fclose(f);
	rename(tmp, status_path);
}

static void set_state(const char *s)
{
	pthread_mutex_lock(&status_lock);
	if (strcmp(state, s)) {
		snprintf(state, sizeof(state), "%s", s);
		status_write_locked();
	}
	pthread_mutex_unlock(&status_lock);
}

static pid_t running_pid(void)
{
	FILE *f = fopen(pid_path, "r");
	int pid = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%d", &pid) != 1)
		pid = 0;
	fclose(f);
	return pid > 0 && kill(pid, 0) == 0 ? pid : 0;
}

/* Stop a running instance and wait for it to let go of the sound card. */
static void stop_running(void)
{
	pid_t pid = running_pid();
	int i;

	if (!pid)
		return;
	kill(pid, SIGTERM);
	for (i = 0; i < 50 && kill(pid, 0) == 0; i++)
		usleep(100 * 1000);
}

/* Bitrate: compressed bytes in against seconds of sound out, over
 * windows of ~4 s. */
static unsigned long long br_bytes, br_frames;

static void bitrate_count(size_t bytes, unsigned long frames)
{
	int kbps;

	br_bytes += bytes;
	br_frames += frames;
	if (pcm_rate <= 0 || br_frames < (unsigned long long)pcm_rate * 4)
		return;
	kbps = (int)(br_bytes * 8 * pcm_rate / br_frames / 1000);
	br_bytes = br_frames = 0;
	if (kbps != bitrate) {
		pthread_mutex_lock(&status_lock);
		bitrate = kbps;
		status_write_locked();
		pthread_mutex_unlock(&status_lock);
	}
}

/* ------------------------------------------------------------------ */
/* Audio out                                                          */

static void pcm_close(void)
{
	if (pcm)
		snd_pcm_close(pcm);
	pcm = NULL;
}

static bool pcm_setup(long rate, int channels)
{
	int i, err = -EBUSY;

	if (pcm && rate == pcm_rate && channels == pcm_channels)
		return true;
	pcm_close();
	/* The FM loop may still be letting go of the card. */
	for (i = 0; i < 30 && !stop; i++) {
		err = snd_pcm_open(&pcm, PCM, SND_PCM_STREAM_PLAYBACK, 0);
		if (err != -EBUSY)
			break;
		usleep(100 * 1000);
	}
	if (err < 0) {
		fprintf(stderr, "chumby-stream: %s: %s\n", PCM, snd_strerror(err));
		pcm = NULL;
		return false;
	}
	/* Half a second of buffer rides out network hiccups. */
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
				 channels, rate, 1, 500000);
	if (err < 0) {
		fprintf(stderr, "chumby-stream: %ld Hz x%d: %s\n", rate, channels,
			snd_strerror(err));
		pcm_close();
		return false;
	}
	pcm_rate = rate;
	pcm_channels = channels;
	return true;
}

static void pcm_write(const unsigned char *buf, size_t bytes)
{
	snd_pcm_sframes_t frames = bytes / (2 * pcm_channels), n;

	while (frames > 0 && pcm && !stop) {
		n = snd_pcm_writei(pcm, buf, frames);
		if (n < 0)
			n = snd_pcm_recover(pcm, n, 1);
		if (n < 0) {
			fprintf(stderr, "chumby-stream: write: %s\n", snd_strerror(n));
			return;
		}
		buf += n * 2 * pcm_channels;
		frames -= n;
		bitrate_count(0, n);
	}
}

/* ------------------------------------------------------------------ */
/* Decoding                                                           */

static void decoder_reset(void)
{
	const long *rates;
	size_t n, i;

	if (mh) {
		mpg123_close(mh);
		mpg123_delete(mh);
	}
	mh = mpg123_new(NULL, NULL);
	if (!mh)
		return;
	mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
	/* 16-bit out at whatever rate the stream has. */
	mpg123_format_none(mh);
	mpg123_rates(&rates, &n);
	for (i = 0; i < n; i++)
		mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
	mpg123_open_feed(mh);
}

static void decode(const char *data, size_t len)
{
	unsigned char out[8192];
	size_t done;
	long rate;
	int channels, enc, r;

	if (!mh || mpg123_feed(mh, (const unsigned char *)data, len) != MPG123_OK)
		return;
	while (!stop) {
		r = mpg123_read(mh, out, sizeof(out), &done);
		if (r == MPG123_NEW_FORMAT) {
			mpg123_getformat(mh, &rate, &channels, &enc);
			if (!pcm_setup(rate, channels))
				stop = 1;	/* no sound card: nothing to do */
			continue;
		}
		if (done && pcm) {
			pcm_write(out, done);
			set_state("playing");
		}
		if (r != MPG123_OK)
			break;		/* MPG123_NEED_MORE, or an error */
	}
}

static void aac_reset(void)
{
	NeAACDecConfigurationPtr cfg;

	if (aac)
		NeAACDecClose(aac);
	aac = NeAACDecOpen();
	aac_ready = false;
	alen = 0;
	if (!aac)
		return;
	cfg = NeAACDecGetCurrentConfiguration(aac);
	cfg->outputFormat = FAAD_FMT_16BIT;
	cfg->downMatrix = 1;		/* 5.1 to stereo */
	NeAACDecSetConfiguration(aac, cfg);
}

/* ADTS frames: a 7 (or 9) byte header with the frame's length, then data. */
static void aac_feed(const unsigned char *d, size_t n)
{
	NeAACDecFrameInfo fi;
	unsigned long rate;
	unsigned char ch;
	size_t take, pos, fl;
	void *out;

	while (n > 0 && !stop && aac) {
		take = n < sizeof(abuf) - alen ? n : sizeof(abuf) - alen;
		memcpy(abuf + alen, d, take);
		alen += take;
		d += take;
		n -= take;
		pos = 0;
		for (;;) {
			while (pos + 1 < alen && !(abuf[pos] == 0xff && (abuf[pos + 1] & 0xf6) == 0xf0))
				pos++;
			if (alen - pos < 7)
				break;
			fl = ((abuf[pos + 3] & 3) << 11) | (abuf[pos + 4] << 3) | (abuf[pos + 5] >> 5);
			if (fl < 7) {
				pos++;
				continue;
			}
			if (alen - pos < fl)
				break;
			if (!aac_ready) {
				if (NeAACDecInit(aac, abuf + pos, fl, &rate, &ch) < 0) {
					pos++;
					continue;
				}
				aac_ready = true;
			}
			out = NeAACDecDecode(aac, &fi, abuf + pos, fl);
			if (!fi.error && fi.samples && out) {
				if (!pcm_setup(fi.samplerate, fi.channels)) {
					stop = 1;	/* no sound card */
					return;
				}
				pcm_write(out, fi.samples * 2);
				set_state("playing");
			}
			pos += fl;
		}
		memmove(abuf, abuf + pos, alen - pos);
		alen -= pos;
	}
}

/* Audio frames: to the decoder for the codec, sniffed from the first
 * frame header if the server didn't say (ADTS has layer bits 00). */
static void es_out(const unsigned char *d, size_t n)
{
	size_t i;

	bitrate_count(n, 0);
	if (codec == CODEC_UNKNOWN) {
		for (i = 0; i + 1 < n; i++)
			if (d[i] == 0xff && (d[i + 1] & 0xe0) == 0xe0) {
				codec = (d[i + 1] & 0x06) == 0 ? CODEC_AAC : CODEC_MP3;
				break;
			}
		if (codec == CODEC_UNKNOWN)
			return;
	}
	if (codec == CODEC_AAC)
		aac_feed(d, n);
	else
		decode((const char *)d, n);
}

/* MPEG-TS: 188-byte packets; the first PID carrying an audio PES
 * (stream_id 0xc0-0xdf) is the one, its PES headers stripped. */
static int ts_pid = -1;
static unsigned char ts_pkt[188];
static size_t ts_len;

static void ts_packet(const unsigned char *p)
{
	int pid = ((p[1] & 0x1f) << 8) | p[2], afc = (p[3] >> 4) & 3;
	size_t off = 4, plen;
	const unsigned char *pl;

	if (afc == 0 || afc == 2)	/* no payload */
		return;
	if (afc == 3)
		off += 1 + p[4];
	if (off >= 188)
		return;
	pl = p + off;
	plen = 188 - off;
	if ((p[1] & 0x40) && plen >= 9 && !pl[0] && !pl[1] && pl[2] == 1) {	/* PES start */
		if (ts_pid < 0 && pl[3] >= 0xc0 && pl[3] <= 0xdf)
			ts_pid = pid;
		if (pid == ts_pid && 9u + pl[8] < plen)
			es_out(pl + 9 + pl[8], plen - 9 - pl[8]);
		return;
	}
	if (pid == ts_pid)
		es_out(pl, plen);
}

static void ts_feed(const unsigned char *d, size_t n)
{
	size_t take;

	while (n > 0 && !stop) {
		if (!ts_len && *d != 0x47) {	/* lost sync */
			d++;
			n--;
			continue;
		}
		take = n < 188 - ts_len ? n : 188 - ts_len;
		memcpy(ts_pkt + ts_len, d, take);
		ts_len += take;
		d += take;
		n -= take;
		if (ts_len == 188) {
			ts_packet(ts_pkt);
			ts_len = 0;
		}
	}
}

/* A whole HLS segment: MPEG-TS, or packed audio behind an ID3 tag. */
static void segment_play(const unsigned char *d, size_t n)
{
	size_t tag;

	if (n > 188 && d[0] == 0x47 && d[188] == 0x47) {
		ts_feed(d, n);
		return;
	}
	if (n > 10 && !memcmp(d, "ID3", 3)) {
		tag = 10 + ((d[6] & 0x7f) << 21 | (d[7] & 0x7f) << 14 | (d[8] & 0x7f) << 7 | (d[9] & 0x7f));
		if (tag >= n)
			return;
		d += tag;
		n -= tag;
	}
	es_out(d, n);
}

static void decoders_reset(void)
{
	decoder_reset();
	aac_reset();
	codec = CODEC_UNKNOWN;
	ts_pid = -1;
	ts_len = 0;
}

/* ------------------------------------------------------------------ */
/* ICY metadata                                                       */

static void icy_parse(void)
{
	char *p = strstr(meta, "StreamTitle='"), *e;

	if (!p)
		return;
	p += strlen("StreamTitle='");
	e = strstr(p, "';");
	if (!e)
		e = p + strlen(p);
	pthread_mutex_lock(&status_lock);
	snprintf(title, sizeof(title), "%.*s", (int)(e - p), p);
	status_write_locked();
	pthread_mutex_unlock(&status_lock);
}

static size_t on_header(char *buf, size_t size, size_t n, void *user)
{
	size_t len = size * n;
	char line[512], *v;

	(void)user;
	snprintf(line, sizeof(line), "%.*s", (int)(len < sizeof(line) ? len : sizeof(line) - 1), buf);
	line[strcspn(line, "\r\n")] = '\0';
	if (!strncmp(line, "HTTP/", 5) || !strncmp(line, "ICY ", 4)) {
		metaint = 0;	/* a new response (redirects) */
		meta_len = -1;
		return len;
	}
	v = strchr(line, ':');
	if (!v)
		return len;
	*v++ = '\0';
	while (*v == ' ')
		v++;
	if (!strcasecmp(line, "content-type")) {
		if (!strncasecmp(v, "audio/aac", 9) || !strncasecmp(v, "audio/x-aac", 11))
			codec = CODEC_AAC;
		else if (!strncasecmp(v, "audio/mpeg", 10))
			codec = CODEC_MP3;
	} else if (!strcasecmp(line, "icy-metaint")) {
		metaint = atol(v);
		audio_left = metaint;
	} else if (!strcasecmp(line, "icy-name") && !got_name && *v) {
		pthread_mutex_lock(&status_lock);
		snprintf(name, sizeof(name), "%s", v);
		status_write_locked();
		pthread_mutex_unlock(&status_lock);
	}
	return len;
}

/* Compressed audio between the network thread and the player. */
#define RING_SIZE	(256 * 1024)	/* ~10 s at 192 kbit/s */
#define PREBUFFER	(48 * 1024)	/* ~2 s at 192 kbit/s, 3 s at 128 */

static unsigned char ring[RING_SIZE];
static size_t ring_head, ring_len;	/* read position, bytes held */
static bool net_done;			/* the transfer ended */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_cond = PTHREAD_COND_INITIALIZER;

/* From the network thread: blocks while the ring is full (TCP then
 * slows the server down). */
static void ring_put(const unsigned char *d, size_t n)
{
	size_t at, take;

	pthread_mutex_lock(&ring_lock);
	while (n > 0 && !stop) {
		while (ring_len == RING_SIZE && !stop)
			pthread_cond_wait(&ring_cond, &ring_lock);
		at = (ring_head + ring_len) % RING_SIZE;
		take = RING_SIZE - ring_len;
		if (take > RING_SIZE - at)
			take = RING_SIZE - at;
		if (take > n)
			take = n;
		memcpy(ring + at, d, take);
		ring_len += take;
		d += take;
		n -= take;
		pthread_cond_broadcast(&ring_cond);
	}
	pthread_mutex_unlock(&ring_lock);
}

static size_t on_data(char *p, size_t size, size_t n, void *user)
{
	size_t len = size * n, i = 0, take;

	(void)user;
	while (i < len && !stop) {
		if (!metaint) {
			ring_put((const unsigned char *)p + i, len - i);
			break;
		}
		if (meta_len < 0 && audio_left > 0) {		/* audio */
			take = len - i < (size_t)audio_left ? len - i : (size_t)audio_left;
			ring_put((const unsigned char *)p + i, take);
			audio_left -= take;
			i += take;
		} else if (meta_len < 0) {			/* length byte */
			meta_len = (unsigned char)p[i++] * 16;
			meta_got = 0;
			if (!meta_len) {
				meta_len = -1;
				audio_left = metaint;
			}
		} else {					/* metadata */
			take = len - i < (size_t)(meta_len - meta_got) ? len - i :
			       (size_t)(meta_len - meta_got);
			memcpy(meta + meta_got, p + i, take);
			meta_got += take;
			i += take;
			if (meta_got == meta_len) {
				meta[meta_len] = '\0';
				icy_parse();
				meta_len = -1;
				audio_left = metaint;
			}
		}
	}
	return stop ? 0 : len;
}

static int on_progress(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	(void)u; (void)a; (void)b; (void)c; (void)d;
	return stop;	/* non-zero aborts the transfer */
}

static CURL *easy(const char *u)
{
	CURL *c = curl_easy_init();

	if (!c)
		return NULL;
	curl_easy_setopt(c, CURLOPT_URL, u);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	return c;
}

/* ------------------------------------------------------------------ */
/* Playlists                                                          */

struct buf {
	char data[16384];
	size_t len;
};

static size_t on_buf(char *p, size_t size, size_t n, void *user)
{
	struct buf *b = user;
	size_t len = size * n, room = sizeof(b->data) - 1 - b->len;

	memcpy(b->data + b->len, p, len < room ? len : room);
	b->len += len < room ? len : room;
	return len;
}

static bool ends_with(const char *u, const char *ext)
{
	const char *q = strpbrk(u, "?#"), *end = q ? q : u + strlen(u);
	size_t n = strlen(ext);

	return (size_t)(end - u) > n && !strncasecmp(end - n, ext, n);
}

static bool is_playlist(const char *u)
{
	return ends_with(u, ".pls") || ends_with(u, ".m3u");
}

/* The first http(s) entry ("File1=..." in .pls, a bare line in .m3u). */
static bool playlist_resolve(const char *u, char *out, size_t size)
{
	static struct buf b;
	CURL *c = easy(u);
	char *line, *save, *p;
	bool ok = false;

	if (!c)
		return false;
	b.len = 0;
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_buf);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
	if (curl_easy_perform(c) == CURLE_OK) {
		b.data[b.len] = '\0';
		for (line = strtok_r(b.data, "\r\n", &save); line && !ok;
		     line = strtok_r(NULL, "\r\n", &save)) {
			p = strstr(line, "http");
			if (p && (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8))) {
				snprintf(out, size, "%s", p);
				ok = true;
			}
		}
	}
	curl_easy_cleanup(c);
	return ok;
}

/* ------------------------------------------------------------------ */
/* HLS                                                                */

struct dyn {
	unsigned char *p;
	size_t len, cap;
};

static size_t on_dyn(char *p, size_t size, size_t n, void *user)
{
	struct dyn *b = user;
	size_t len = size * n;

	if (b->len + len + 1 > b->cap) {
		size_t cap = (b->len + len + 1) * 2;
		unsigned char *q = realloc(b->p, cap);

		if (!q)
			return 0;
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->len, p, len);
	b->len += len;
	b->p[b->len] = '\0';
	return len;
}

/* GET into b on the kept-alive handle. */
static bool fetch(CURL *c, const char *u, struct dyn *b, long timeout)
{
	long code = 0;

	b->len = 0;
	curl_easy_setopt(c, CURLOPT_URL, u);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_dyn);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
	if (curl_easy_perform(c) != CURLE_OK)
		return false;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	return code == 200 && b->len;
}

/* A playlist entry against the playlist's own URL. */
static void resolve(const char *base, const char *ref, char *out, size_t size)
{
	const char *q, *host;
	size_t n;

	if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8)) {
		snprintf(out, size, "%s", ref);
		return;
	}
	host = strstr(base, "://");
	host = host ? host + 3 : base;
	if (ref[0] == '/') {			/* from the host's root */
		n = strcspn(host, "/?#");
		snprintf(out, size, "%.*s%s", (int)(host + n - base), base, ref);
		return;
	}
	q = base + strcspn(base, "?#");		/* beside the playlist */
	while (q > host && q[-1] != '/')
		q--;
	snprintf(out, size, "%.*s%s", (int)(q - base), base, ref);
}

struct seg {
	struct seg *next;
	size_t len;
	unsigned char data[];
};

#define QUEUE_MAX	8	/* segments fetched ahead (~16 s at 2 s each) */

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static struct seg *q_head, *q_tail;
static int q_count;
static bool hls_done;			/* the fetcher gave up or the stream ended */
static char hls_url[1024];

static void q_push(struct seg *g)
{
	pthread_mutex_lock(&q_lock);
	while (q_count >= QUEUE_MAX && !stop)
		pthread_cond_wait(&q_cond, &q_lock);
	g->next = NULL;
	if (q_tail)
		q_tail->next = g;
	else
		q_head = g;
	q_tail = g;
	q_count++;
	pthread_cond_broadcast(&q_cond);
	pthread_mutex_unlock(&q_lock);
}

/* The next segment, waiting up to half a second. */
static struct seg *q_pop(void)
{
	struct timespec ts;
	struct seg *g;

	pthread_mutex_lock(&q_lock);
	if (!q_head && !hls_done && !stop) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 500 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&q_cond, &q_lock, &ts);
	}
	g = q_head;
	if (g) {
		q_head = g->next;
		if (!q_head)
			q_tail = NULL;
		q_count--;
		pthread_cond_broadcast(&q_cond);
	}
	pthread_mutex_unlock(&q_lock);
	return g;
}

static void q_clear(void)
{
	struct seg *g;

	pthread_mutex_lock(&q_lock);
	while ((g = q_head)) {
		q_head = g->next;
		free(g);
	}
	q_tail = NULL;
	q_count = 0;
	pthread_mutex_unlock(&q_lock);
}

#define MAX_SEGS	128

/* Follow the (live) playlist: fetch each new segment once, in order.
 * Starts three segments back from the live edge, for some slack. */
static void *hls_fetcher(void *arg)
{
	char pl_url[1024], seg_url[1024], *line, *save, *uris[MAX_SEGS];
	struct dyn pl = { 0 }, sb = { 0 };
	long long seq0, last = -1;
	int n, i, start, fails = 0, target;
	bool endlist, fresh;
	CURL *c = easy(hls_url);
	struct seg *g;

	(void)arg;
	snprintf(pl_url, sizeof(pl_url), "%s", hls_url);
	while (c && !stop && fails < 5) {
		if (!fetch(c, pl_url, &pl, 20)) {
			fails++;
			sleep_s(1);
			continue;
		}
		if (!strstr((char *)pl.p, "#EXTM3U"))
			break;
		if (strstr((char *)pl.p, "#EXT-X-MAP")) {
			fprintf(stderr, "chumby-stream: fMP4 HLS isn't supported\n");
			break;
		}
		seq0 = 0;
		target = 2;
		endlist = strstr((char *)pl.p, "#EXT-X-ENDLIST") != NULL;
		n = 0;
		/* A master playlist: take its first variant. */
		if (strstr((char *)pl.p, "#EXT-X-STREAM-INF")) {
			bool next = false;

			for (line = strtok_r((char *)pl.p, "\r\n", &save); line;
			     line = strtok_r(NULL, "\r\n", &save)) {
				if (!strncmp(line, "#EXT-X-STREAM-INF", 17))
					next = true;
				else if (next && line[0] && line[0] != '#') {
					resolve(pl_url, line, seg_url, sizeof(seg_url));
					snprintf(pl_url, sizeof(pl_url), "%s", seg_url);
					break;
				}
			}
			continue;
		}
		for (line = strtok_r((char *)pl.p, "\r\n", &save); line && n < MAX_SEGS;
		     line = strtok_r(NULL, "\r\n", &save)) {
			if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22))
				seq0 = atoll(line + 22);
			else if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22))
				target = atoi(line + 22);
			else if (line[0] && line[0] != '#')
				uris[n++] = line;
		}
		if (last < 0 || last + 1 < seq0 || last + 1 > seq0 + n)
			start = n > 3 ? n - 3 : 0;	/* first time, or fell behind */
		else
			start = last + 1 - seq0;
		fresh = false;
		for (i = start; i < n && !stop; i++) {
			resolve(pl_url, uris[i], seg_url, sizeof(seg_url));
			if (!fetch(c, seg_url, &sb, 30)) {
				fails++;
				break;
			}
			g = malloc(sizeof(*g) + sb.len);
			if (!g)
				break;
			g->len = sb.len;
			memcpy(g->data, sb.p, sb.len);
			q_push(g);
			last = seq0 + i;
			fresh = true;
			fails = 0;
		}
		if (endlist && !fresh)
			break;
		if (!fresh)	/* nothing new yet: half a segment */
			sleep_s(target > 2 ? target / 2 : 1);
	}
	if (c)
		curl_easy_cleanup(c);
	free(pl.p);
	free(sb.p);
	pthread_mutex_lock(&q_lock);
	hls_done = true;
	pthread_cond_broadcast(&q_cond);
	pthread_mutex_unlock(&q_lock);
	return NULL;
}

/* Play an HLS stream until it ends, fails or we're stopped. */
static void hls_play(const char *u)
{
	pthread_t t;
	struct seg *g;

	snprintf(hls_url, sizeof(hls_url), "%s", u);
	hls_done = false;
	if (pthread_create(&t, NULL, hls_fetcher, NULL))
		return;
	while (!stop) {
		g = q_pop();
		if (g) {
			segment_play(g->data, g->len);
			free(g);
		} else if (hls_done) {
			break;
		}
	}
	pthread_mutex_lock(&q_lock);
	pthread_cond_broadcast(&q_cond);	/* a fetcher waiting for room */
	pthread_mutex_unlock(&q_lock);
	pthread_join(t, NULL);
	q_clear();
}

/* ------------------------------------------------------------------ */

static void sleep_s(int s)
{
	int i;

	for (i = 0; i < s * 10 && !stop; i++)
		usleep(100 * 1000);
}

static CURLcode net_res;

static void *net_thread(void *arg)
{
	net_res = curl_easy_perform(arg);
	pthread_mutex_lock(&ring_lock);
	net_done = true;
	pthread_cond_broadcast(&ring_cond);
	pthread_mutex_unlock(&ring_lock);
	return NULL;
}

/* Play from the ring until the transfer ends (or we're stopped). Wait
 * for PREBUFFER first, and again whenever it runs dry. */
static CURLcode stream_play(CURL *c)
{
	static unsigned char chunk[4096];
	bool filled = false;
	struct timespec ts;
	pthread_t t;
	size_t n;

	ring_head = ring_len = 0;
	net_done = false;
	if (pthread_create(&t, NULL, net_thread, c))
		return CURLE_FAILED_INIT;
	while (!stop) {
		pthread_mutex_lock(&ring_lock);
		while (!stop && !net_done && ring_len < (filled ? 1 : PREBUFFER)) {
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 200 * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&ring_cond, &ring_lock, &ts);
		}
		if (!ring_len) {
			pthread_mutex_unlock(&ring_lock);
			if (net_done || stop)
				break;
			continue;
		}
		filled = true;
		n = ring_len < sizeof(chunk) ? ring_len : sizeof(chunk);
		if (n > RING_SIZE - ring_head)
			n = RING_SIZE - ring_head;
		memcpy(chunk, ring + ring_head, n);
		ring_head = (ring_head + n) % RING_SIZE;
		ring_len -= n;
		if (!ring_len)
			filled = false;		/* ran dry: buffer up again */
		pthread_cond_broadcast(&ring_cond);
		pthread_mutex_unlock(&ring_lock);
		es_out(chunk, n);
	}
	pthread_mutex_lock(&ring_lock);
	pthread_cond_broadcast(&ring_cond);	/* a writer waiting for room */
	pthread_mutex_unlock(&ring_lock);
	pthread_join(t, NULL);
	return net_res;
}

static void play(void)
{
	char target[1024];
	int backoff = 2;
	CURLcode res;
	CURL *c;

	while (!stop) {
		set_state("connecting");
		snprintf(target, sizeof(target), "%s", url);
		if (is_playlist(url) && !playlist_resolve(url, target, sizeof(target))) {
			set_state("error");
			sleep_s(backoff);
			backoff = backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff * 2;
			continue;
		}
		decoders_reset();
		if (ends_with(target, ".m3u8")) {
			hls_play(target);
			if (stop)
				break;
			res = CURLE_RECV_ERROR;
			goto failed;
		}
		metaint = 0;
		meta_len = -1;
		c = easy(target);
		if (!c)
			break;
		{
			struct curl_slist *h = curl_slist_append(NULL, "Icy-MetaData: 1");

			curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
			curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
			/* A stalled stream: under 1 kB/s for 20 s. */
			curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1000L);
			curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
			res = stream_play(c);
			curl_slist_free_all(h);
		}
		curl_easy_cleanup(c);
		if (stop)
			break;
failed:
		if (!strcmp(state, "playing"))
			backoff = 2;	/* it was working: retry soon */
		fprintf(stderr, "chumby-stream: %s: %s\n", target, curl_easy_strerror(res));
		set_state("error");
		sleep_s(backoff);
		backoff = backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff * 2;
	}
}

int main(int argc, char **argv)
{
	const char *dir = chumby_run_dir();
	char *audio_argv[] = { "chumby-audio", "play", "stream", NULL };
	bool resuming = false;
	struct sigaction sa;
	pid_t pid;
	FILE *f;

	snprintf(status_path, sizeof(status_path), "%s/stream", dir);
	snprintf(pid_path, sizeof(pid_path), "%s/stream.pid", dir);
	snprintf(paused_path, sizeof(paused_path), "%s/stream.paused", dir);
	if (argc < 2) {
		fprintf(stderr, "usage: chumby-stream <url> [name] | stop | pause | resume | active\n");
		return 2;
	}
	if (!strcmp(argv[1], "active"))
		return running_pid() ? 0 : 1;
	if (!strcmp(argv[1], "pause")) {
		char line[1200];
		FILE *st;

		if (!running_pid())
			return 0;
		/* What plays, to come back to: its URL and name. */
		st = fopen(status_path, "r");
		f = fopen(paused_path, "w");
		while (st && f && fgets(line, sizeof(line), st))
			if (!strncmp(line, "url=", 4) || !strncmp(line, "name=", 5))
				fputs(line, f);
		if (st)
			fclose(st);
		if (f)
			fclose(f);
		stop_running();
		unlink(status_path);
		return 0;
	}
	if (!strcmp(argv[1], "resume")) {
		char line[1200];

		f = fopen(paused_path, "r");
		if (!f)
			return 0;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = '\0';
			if (!strncmp(line, "url=", 4))
				snprintf(url, sizeof(url), "%.1023s", line + 4);
			else if (!strncmp(line, "name=", 5) && line[5]) {
				snprintf(name, sizeof(name), "%.127s", line + 5);
				got_name = true;
			}
		}
		fclose(f);
		unlink(paused_path);
		if (!url[0])
			return 0;
		resuming = true;
	}
	stop_running();
	if (!strcmp(argv[1], "stop")) {
		unlink(status_path);
		unlink(paused_path);
		return 0;
	}
	if (!resuming) {
		snprintf(url, sizeof(url), "%s", argv[1]);
		if (argc > 2 && argv[2][0]) {
			snprintf(name, sizeof(name), "%s", argv[2]);
			got_name = true;
		}
		unlink(paused_path);
		/* Whatever else plays (the FM radio) stops: one at a time. A
		 * resume comes from chumby-audio itself (it holds the lock). */
		pid = fork();
		if (pid == 0) {
			execv(AUDIO, audio_argv);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}

	/* Into the background, on our own (closing the app leaves us on). */
	pid = fork();
	if (pid < 0)
		return 1;
	if (pid > 0)
		return 0;
	setsid();
	setpriority(PRIO_PROCESS, 0, -10);
	mkdir(dir, 0755);
	f = fopen(pid_path, "w");
	if (f) {
		fprintf(f, "%d\n", getpid());
		fclose(f);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	mpg123_init();
	play();

	pcm_close();
	if (mh)
		mpg123_delete(mh);
	if (aac)
		NeAACDecClose(aac);
	unlink(status_path);
	unlink(pid_path);
	return 0;
}
