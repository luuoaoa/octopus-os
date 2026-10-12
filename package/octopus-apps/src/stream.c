// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * octopus-stream: play an internet radio stream.
 *
 *   octopus-stream <url> [name] [byte]
 *                                start playing (replacing any stream), in
 *                                the background; takes the sound from
 *                                other sources (octopus-audio play stream).
 *                                A file (a podcast episode) can start at a
 *                                byte (an HTTP range: resuming it)
 *   octopus-stream stop
 *   octopus-stream pause | resume   for octopus-audio: stop for a while,
 *                                remembering what played; play it again
 *   octopus-stream active         exit status: playing?
 *
 * Over http or https (libcurl), to the codec (ALSA, as the FM loop uses
 * it):
 *   - Shoutcast/Icecast streams, MP3 (libmpg123) or AAC (faad2, fixed
 *     point: no FPU here); their "ICY" metadata gives what's playing.
 *     A thread downloads into a buffer of a few seconds, and playing
 *     starts (again, after running dry) once ~2 s are in (by the
 *     server's icy-br, else 128k): Wi-Fi hiccups don't reach the speaker
 *   - HLS (.m3u8): MPEG-TS or packed-audio segments of either, fetched
 *     by a thread on one kept-alive connection (a TLS handshake per 2 s
 *     segment would be too slow on this CPU) and queued for playing
 *   - .pls and .m3u playlists, followed to their first entry
 *   - MP4 audio files (.m4a, a podcast's AAC): their index (moov) read
 *     first - from the end, by a range request, if it's there - then
 *     each AAC frame from where the index says
 * A dropped stream is reconnected, backing off up to 30 s. We run above
 * the UI's priority, so redraws can't make the decoder late.
 *
 * State goes to $run_dir/stream for the app ("state=", "url=", "name=",
 * "title=", "codec=" MP3/AAC, "bitrate=" kbit/s as measured over a few
 * seconds, "start=" the byte this run was started at: the app tells its
 * own run from the one it replaced) lines, gone once stopped; our pid to
 * $run_dir/stream.pid.
 * Starting, stopping, pausing and resuming hold $run_dir/stream.lock until
 * the new player's pid is in place: two starts close together (seeks)
 * would otherwise each find nothing to stop yet, and both play.
 *
 * A file rather than a live stream (a Content-Length and no ICY: a podcast
 * episode) also gets "byte=" (how far into it is played: resume from
 * there), "length=" (its size) and "played=" (seconds played this time),
 * written about once a second. A drop mid-file resumes from there; at its
 * end it's "state=ended" - and that status stays, for the app to see.
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
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <octopus.h>

/* The board's own PCM if its asound.conf names one (the RasPad: HDMI,
 * with a soft volume), else the first card, straight. */
#define PCM_BOARD	"octopus"
#define PCM		"plughw:0,0"
#define CA_BUNDLE	"/etc/ssl/certs/ca-certificates.crt"
#define AUDIO		"/usr/bin/octopus-audio"
#define USER_AGENT	"octopus-stream/1.0"
#define BACKOFF_MAX_S	30

static volatile sig_atomic_t stop;
static char status_path[256], pid_path[256], paused_path[256], lock_path[256];
static int bitrate;			/* kbit/s, measured */
static char url[1024], name[128], title[256], state[16];

/* A file (see above): where in it, how long, how much played. */
static bool finite;			/* this response is one */
static long long start_byte;		/* asked for (a range) */
static long long run_start;		/* what this run was started at ("start="), as asked */
static long long base_byte;		/* where this response starts */
static long long fed;			/* bytes of it to the decoders */
static long long file_len;		/* its whole size */
static unsigned long long played_frames;
static time_t pos_written;


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
static int icy_br;			/* the server's stated bitrate, kbit/s */

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

enum codec { CODEC_UNKNOWN, CODEC_MP3, CODEC_AAC, CODEC_MP4 };
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
	fprintf(f, "state=%s\nurl=%s\nname=%s\ntitle=%s\ncodec=%s\nbitrate=%d\nstart=%lld\n",
		state, url, name, title, codec == CODEC_AAC || codec == CODEC_MP4 ? "AAC" :
		codec == CODEC_MP3 ? "MP3" : "", bitrate, run_start);
	if (finite)
		fprintf(f, "byte=%lld\nlength=%lld\nplayed=%llu\n", base_byte + fed, file_len,
			pcm_rate > 0 ? played_frames / (unsigned long long)pcm_rate : 0ULL);
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

/* Stop a running instance and wait for it to let go of the sound card
 * (killed if it won't, in 5 s: it mustn't play on beside the next). */
static void stop_running(void)
{
	pid_t pid = running_pid();
	int i;

	if (!pid)
		return;
	kill(pid, SIGTERM);
	for (i = 0; i < 50 && kill(pid, 0) == 0; i++)
		usleep(100 * 1000);
	if (kill(pid, 0) == 0) {
		kill(pid, SIGKILL);
		for (i = 0; i < 10 && kill(pid, 0) == 0; i++)
			usleep(100 * 1000);
	}
}

/* Is the pid file (still) ours? Not once we're replaced. */
static bool pid_is_ours(void)
{
	FILE *f = fopen(pid_path, "r");
	int pid = 0;

	if (!f)
		return false;
	if (fscanf(f, "%d", &pid) != 1)
		pid = 0;
	fclose(f);
	return pid == getpid();
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
		err = snd_pcm_open(&pcm, PCM_BOARD, SND_PCM_STREAM_PLAYBACK, 0);
		if (err == -ENOENT)	/* not defined here */
			err = snd_pcm_open(&pcm, PCM, SND_PCM_STREAM_PLAYBACK, 0);
		if (err != -EBUSY)
			break;
		usleep(100 * 1000);
	}
	if (err < 0) {
		fprintf(stderr, "octopus-stream: %s: %s\n", PCM, snd_strerror(err));
		pcm = NULL;
		return false;
	}
	/* Half a second of buffer rides out hiccups. Step by step with the
	 * "near" calls: snd_pcm_set_params() gives up on rates the codec
	 * must resample, like 24 kHz (BBC World Service) - no period size
	 * fits exactly. */
	{
		snd_pcm_hw_params_t *hw;
		unsigned int r = rate, bt = 500000, pt = bt / 4;

		snd_pcm_hw_params_alloca(&hw);
		err = snd_pcm_hw_params_any(pcm, hw);
		if (err >= 0)
			err = snd_pcm_hw_params_set_rate_resample(pcm, hw, 1);
		if (err >= 0)
			err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
		if (err >= 0)
			err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16);
		if (err >= 0)
			err = snd_pcm_hw_params_set_channels(pcm, hw, channels);
		if (err >= 0)
			err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, NULL);
		if (err >= 0)
			err = snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &bt, NULL);
		if (err >= 0)
			err = snd_pcm_hw_params_set_period_time_near(pcm, hw, &pt, NULL);
		if (err >= 0)
			err = snd_pcm_hw_params(pcm, hw);
		if (err >= 0 && r != (unsigned int)rate)
			err = -EINVAL;	/* would play at the wrong speed */
	}
	if (err < 0) {
		fprintf(stderr, "octopus-stream: %ld Hz x%d: %s\n", rate, channels,
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
			/* Not an underrun: the device went (PipeWire restarted, on
			 * the RasPad). Reopen it with the next write, not spin. */
			fprintf(stderr, "octopus-stream: write: %s; reopening\n",
				snd_strerror(n));
			pcm_close();
			usleep(500 * 1000);
			return;
		}
		buf += n * 2 * pcm_channels;
		frames -= n;
		bitrate_count(0, n);
		played_frames += n;
	}
	if (finite && time(NULL) != pos_written) {
		pos_written = time(NULL);
		pthread_mutex_lock(&status_lock);
		status_write_locked();
		pthread_mutex_unlock(&status_lock);
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
/* ------------------------------------------------------------------ */
/* MP4 (.m4a): an index (moov) of where each AAC frame is, then the     */
/* frames (in mdat). Kept across reconnects: a resume goes straight to   */
/* a frame.                                                             */

#define MOOV_MAX	(16 << 20)

static struct {
	bool indexed;			/* moov read: the arrays below */
	unsigned char asc[64];		/* AudioSpecificConfig, for faad */
	int asc_len;
	uint32_t n;			/* frames */
	long long *off;			/* each one's place in the file */
	uint32_t *size;
	uint32_t next;			/* the next to decode */
	unsigned char *part;		/* one cut by a chunk boundary */
	size_t part_have;
	bool dec_ready;
	/* Boxes, as they stream by. */
	enum { BOX_HEAD, BOX_SKIP, BOX_MOOV, BOX_MDAT } state;
	unsigned char head[16];
	int head_have;
	long long remain;		/* of the box (-1: to the end) */
	unsigned char *moov;
	size_t moov_len, moov_have;
	long long want_byte;		/* resume here, once indexed */
} m4;
static volatile bool restart_xfer;	/* a new range: this transfer's done */
static char cur_target[1024];		/* what's being fetched */

static uint32_t be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint64_t be64(const unsigned char *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

/* The box of type t directly inside [p, end), or NULL; *len its payload. */
static const unsigned char *box_find(const unsigned char *p, const unsigned char *end,
				     const char *t, size_t *len)
{
	while (end - p >= 8) {
		uint64_t sz = be32(p);
		size_t hl = 8;

		if (sz == 1 && end - p >= 16) {
			sz = be64(p + 8);
			hl = 16;
		} else if (sz == 0) {
			sz = end - p;
		}
		if (sz < hl || sz > (uint64_t)(end - p))
			return NULL;
		if (!memcmp(p + 4, t, 4)) {
			*len = sz - hl;
			return p + hl;
		}
		p += sz;
	}
	return NULL;
}

/* An MPEG-4 descriptor's length (1-4 bytes, 7 bits each). */
static size_t desc_len(const unsigned char **p, const unsigned char *end)
{
	size_t v = 0;
	int i;

	for (i = 0; i < 4 && *p < end; i++) {
		unsigned char b = *(*p)++;

		v = v << 7 | (b & 0x7f);
		if (!(b & 0x80))
			break;
	}
	return v;
}

/* esds: ES_Descriptor > DecoderConfigDescriptor > DecSpecificInfo. */
static bool esds_asc(const unsigned char *p, size_t len)
{
	const unsigned char *end = p + len;
	unsigned char flags;

	p += 4;				/* version, flags */
	if (p >= end || *p++ != 3)
		return false;
	desc_len(&p, end);
	if (end - p < 3)
		return false;
	p += 2;				/* ES_ID */
	flags = *p++;
	if (flags & 0x80)
		p += 2;
	if ((flags & 0x40) && p < end)
		p += 1 + *p;
	if (flags & 0x20)
		p += 2;
	if (p >= end || *p++ != 4)
		return false;
	desc_len(&p, end);
	p += 13;			/* object type ... average bitrate */
	if (p >= end || *p++ != 5)
		return false;
	len = desc_len(&p, end);
	if (!len || len > sizeof(m4.asc) || (size_t)(end - p) < len)
		return false;
	memcpy(m4.asc, p, len);
	m4.asc_len = (int)len;
	return true;
}

/* The first sound track's AAC: its config and every frame's place. */
static bool moov_parse(const unsigned char *moov, size_t len)
{
	const unsigned char *end = moov + len, *trak = moov;
	size_t tl;

	while ((trak = box_find(trak, end, "trak", &tl))) {
		const unsigned char *tend = trak + tl, *mdia, *hdlr, *minf, *stbl, *stsd, *e;
		const unsigned char *stsz, *stco, *stsc;
		size_t ml, hl, il, bl, dl, zl, cl, sl, el;
		uint32_t n_chunks, n_stsc, fixed, i, c, k, smp;
		bool co64 = false;

		mdia = box_find(trak, tend, "mdia", &ml);
		trak = tend;		/* the next one, if this isn't it */
		if (!mdia)
			continue;
		hdlr = box_find(mdia, mdia + ml, "hdlr", &hl);
		if (!hdlr || hl < 12 || memcmp(hdlr + 8, "soun", 4))
			continue;
		minf = box_find(mdia, mdia + ml, "minf", &il);
		stbl = minf ? box_find(minf, minf + il, "stbl", &bl) : NULL;
		stsd = stbl ? box_find(stbl, stbl + bl, "stsd", &dl) : NULL;
		if (!stsd || dl < 8 + 36)
			continue;
		/* stsd: version/flags, count, then the mp4a entry. */
		e = box_find(stsd + 8, stsd + dl, "mp4a", &el);
		if (!e || el < 28)
			continue;
		{
			/* The sample entry: 28 bytes (version 0), 44 (1), 64 (2). */
			unsigned v = e[8] << 8 | e[9];
			size_t skip = v == 1 ? 44 : v == 2 ? 64 : 28, xl;
			const unsigned char *esds = el > skip ?
				box_find(e + skip, e + el, "esds", &xl) : NULL;

			if (!esds || !esds_asc(esds, xl))
				continue;
		}
		stsz = box_find(stbl, stbl + bl, "stsz", &zl);
		stsc = box_find(stbl, stbl + bl, "stsc", &sl);
		stco = box_find(stbl, stbl + bl, "stco", &cl);
		if (!stco) {
			stco = box_find(stbl, stbl + bl, "co64", &cl);
			co64 = true;
		}
		if (!stsz || zl < 12 || !stsc || sl < 8 || !stco || cl < 8)
			continue;
		fixed = be32(stsz + 4);
		m4.n = be32(stsz + 8);
		n_stsc = be32(stsc + 4);
		n_chunks = be32(stco + 4);
		if (!m4.n || (!fixed && zl < 12 + 4ull * m4.n) || sl < 8 + 12ull * n_stsc ||
		    cl < 8 + (co64 ? 8ull : 4ull) * n_chunks)
			continue;
		m4.off = calloc(m4.n, sizeof(*m4.off));
		m4.size = calloc(m4.n, sizeof(*m4.size));
		if (!m4.off || !m4.size)
			return false;
		for (i = 0; i < m4.n; i++)
			m4.size[i] = fixed ? fixed : be32(stsz + 12 + 4 * i);
		/* Chunks: each n frames back to back from its offset. */
		for (c = 0, smp = 0, k = 0; c < n_chunks && smp < m4.n; c++) {
			long long o = co64 ? (long long)be64(stco + 8 + 8 * c) : be32(stco + 8 + 4 * c);
			uint32_t per;

			while (k + 1 < n_stsc && be32(stsc + 8 + 12 * (k + 1)) <= c + 1)
				k++;
			per = be32(stsc + 8 + 12 * k + 4);
			for (i = 0; i < per && smp < m4.n; i++, smp++) {
				m4.off[smp] = o;
				o += m4.size[smp];
			}
		}
		m4.n = smp;
		m4.indexed = m4.n > 0;
		return m4.indexed;
	}
	return false;
}

/* The first frame at or after a byte. */
static uint32_t mp4_frame_at(long long byte)
{
	uint32_t lo = 0, hi = m4.n;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (m4.off[mid] < byte)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

struct tail {
	unsigned char *p;
	size_t len, cap;
};

static size_t on_tail(char *p, size_t size, size_t n, void *user)
{
	struct tail *b = user;
	size_t len = size * n;

	if (b->len + len > MOOV_MAX + 4096)
		return 0;
	if (b->len + len > b->cap) {
		size_t cap = (b->len + len) * 2;
		unsigned char *q = realloc(b->p, cap);

		if (!q)
			return 0;
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->len, p, len);
	b->len += len;
	return len;
}

/* The index after the audio (no "fast start"): fetch it from there. */
static bool moov_fetch_tail(long long from)
{
	struct tail b = { 0 };
	const unsigned char *moov;
	size_t ml;
	char range[48];
	bool ok = false;
	CURL *c = curl_easy_init();

	if (!c)
		return false;
	snprintf(range, sizeof(range), "%lld-", from);
	curl_easy_setopt(c, CURLOPT_URL, cur_target);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(c, CURLOPT_RANGE, range);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_tail);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
	if (curl_easy_perform(c) == CURLE_OK && b.len) {
		moov = box_find(b.p, b.p + b.len, "moov", &ml);
		ok = moov && moov_parse(moov, ml);
	}
	curl_easy_cleanup(c);
	free(b.p);
	return ok;
}

static void mp4_decode(const unsigned char *f, uint32_t len)
{
	NeAACDecFrameInfo fi;
	unsigned long rate;
	unsigned char ch;
	void *out;

	if (!aac)
		return;
	if (!m4.dec_ready) {
		if (NeAACDecInit2(aac, m4.asc, m4.asc_len, &rate, &ch) < 0)
			return;
		m4.dec_ready = true;
	}
	out = NeAACDecDecode(aac, &fi, (unsigned char *)f, len);
	if (!fi.error && fi.samples && out) {
		if (!pcm_setup(fi.samplerate, fi.channels)) {
			stop = 1;	/* no sound card */
			return;
		}
		pcm_write(out, fi.samples * 2);
		set_state("playing");
	}
}

/* Bytes of mdat at file offset abs: the frames in them, whole. */
static void mp4_frames(const unsigned char *d, size_t n, long long abs)
{
	long long end = abs + (long long)n;

	while (m4.next < m4.n && !stop && !restart_xfer) {
		long long fo = m4.off[m4.next];
		uint32_t fs = m4.size[m4.next];

		if (m4.part_have) {		/* one begun in an earlier piece */
			size_t take = fs - m4.part_have;

			if (abs + (long long)take > end)
				take = (size_t)(end - abs);
			memcpy(m4.part + m4.part_have, d + (fo + (long long)m4.part_have - abs), take);
			m4.part_have += take;
			if (m4.part_have < fs)
				return;
			mp4_decode(m4.part, fs);
			m4.part_have = 0;
			m4.next++;
			continue;
		}
		if (fo >= end)
			return;			/* in a later piece */
		if (fo < abs) {			/* began before we got here: skip */
			m4.next++;
			continue;
		}
		if (fo + fs <= end) {
			mp4_decode(d + (fo - abs), fs);
			m4.next++;
			continue;
		}
		free(m4.part);			/* cut: keep its start */
		m4.part = malloc(fs);
		if (!m4.part)
			return;
		m4.part_have = (size_t)(end - fo);
		memcpy(m4.part, d + (fo - abs), m4.part_have);
		return;
	}
}

/* The file as it comes, from offset abs: boxes, the index, the frames. */
static void mp4_feed(const unsigned char *d, size_t n, long long abs)
{
	while (n > 0 && !stop && !restart_xfer) {
		size_t take;

		switch (m4.state) {
		case BOX_HEAD: {
			uint64_t sz;
			int need;

			take = n < (size_t)(8 - m4.head_have) ? n : (size_t)(8 - m4.head_have);
			if (m4.head_have >= 8)
				take = n < (size_t)(16 - m4.head_have) ? n : (size_t)(16 - m4.head_have);
			memcpy(m4.head + m4.head_have, d, take);
			m4.head_have += take;
			d += take;
			n -= take;
			abs += take;
			if (m4.head_have < 8)
				return;
			sz = be32(m4.head);
			need = sz == 1 ? 16 : 8;
			if (m4.head_have < need)
				continue;
			if (sz == 1)
				sz = be64(m4.head + 8);
			m4.head_have = 0;
			m4.remain = sz == 0 ? -1 : (long long)sz - need;
			if (!memcmp(m4.head + 4, "moov", 4) && !m4.indexed && m4.remain > 0 &&
			    m4.remain < MOOV_MAX) {
				m4.moov_len = (size_t)m4.remain;
				m4.moov_have = 0;
				m4.moov = malloc(m4.moov_len);
				m4.state = m4.moov ? BOX_MOOV : BOX_SKIP;
			} else if (!memcmp(m4.head + 4, "mdat", 4)) {
				m4.state = BOX_MDAT;
				if (!m4.indexed) {
					/* The index comes after: get it now. */
					if (m4.remain < 0 || !moov_fetch_tail(abs + m4.remain)) {
						fprintf(stderr, "octopus-stream: no index in this MP4\n");
						stop = 1;
						return;
					}
					/* Resuming further in: a new range, straight to it. */
					if (m4.want_byte > abs + (1 << 16)) {
						restart_xfer = true;
						return;
					}
					m4.next = mp4_frame_at(abs);
				}
			} else {
				m4.state = BOX_SKIP;
			}
			break;
		}
		case BOX_SKIP:
			take = m4.remain < 0 || (long long)n < m4.remain ? n : (size_t)m4.remain;
			d += take;
			n -= take;
			abs += take;
			m4.remain -= m4.remain < 0 ? 0 : (long long)take;
			if (!m4.remain)
				m4.state = BOX_HEAD;
			break;
		case BOX_MOOV:
			take = n < m4.moov_len - m4.moov_have ? n : m4.moov_len - m4.moov_have;
			memcpy(m4.moov + m4.moov_have, d, take);
			m4.moov_have += take;
			d += take;
			n -= take;
			abs += take;
			if (m4.moov_have < m4.moov_len)
				break;
			moov_parse(m4.moov, m4.moov_len);
			free(m4.moov);
			m4.moov = NULL;
			m4.state = BOX_HEAD;
			if (!m4.indexed) {
				fprintf(stderr, "octopus-stream: no AAC track in this MP4\n");
				stop = 1;
				return;
			}
			/* Resuming further in: a new range, straight to it. */
			if (m4.want_byte > abs + (1 << 16)) {
				restart_xfer = true;
				return;
			}
			m4.next = mp4_frame_at(abs);
			break;
		case BOX_MDAT:
			take = m4.remain < 0 || (long long)n < m4.remain ? n : (size_t)m4.remain;
			mp4_frames(d, take, abs);
			d += take;
			n -= take;
			abs += take;
			m4.remain -= m4.remain < 0 ? 0 : (long long)take;
			if (!m4.remain)
				m4.state = BOX_HEAD;
			break;
		}
	}
}

static bool ends_with(const char *u, const char *ext);

static bool looks_mp4(const char *u)
{
	return ends_with(u, ".m4a") || ends_with(u, ".mp4") || ends_with(u, ".m4b");
}

static void es_out(const unsigned char *d, size_t n)
{
	size_t i;

	long long abs = base_byte + fed;	/* where d is in the file */

	bitrate_count(n, 0);
	fed += n;
	if (codec == CODEC_UNKNOWN && n >= 3 && !memcmp(d, "ID3", 3))
		codec = CODEC_MP3;	/* a podcast's tag: its picture could look like AAC */
	if (codec == CODEC_UNKNOWN && n >= 8 && !memcmp(d + 4, "ftyp", 4))
		codec = CODEC_MP4;
	if (codec == CODEC_MP4) {
		mp4_feed(d, n, abs);
		return;
	}
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
		const char *code = strchr(line, ' ');

		metaint = 0;	/* a new response (redirects) */
		meta_len = -1;
		finite = false;
		/* Our range honoured (206), or the whole file again (200). */
		base_byte = code && atoi(code + 1) == 206 ? start_byte : 0;
		fed = 0;
		return len;
	}
	v = strchr(line, ':');
	if (!v)
		return len;
	*v++ = '\0';
	while (*v == ' ')
		v++;
	if (!strcasecmp(line, "icy-br")) {
		icy_br = atoi(v);
	} else if (!strcasecmp(line, "content-length")) {
		finite = atoll(v) > 0;		/* until ICY says otherwise */
		if (!file_len || base_byte == 0)
			file_len = base_byte + atoll(v);
	} else if (!strcasecmp(line, "content-range")) {
		const char *total = strchr(v, '/');	/* bytes a-b/total */

		if (total && atoll(total + 1) > 0)
			file_len = atoll(total + 1);
	} else if (!strcasecmp(line, "content-type")) {
		if (!strncasecmp(v, "audio/aac", 9) || !strncasecmp(v, "audio/x-aac", 11))
			codec = CODEC_AAC;
		else if (!strncasecmp(v, "audio/mpeg", 10))
			codec = CODEC_MP3;
		else if (!strncasecmp(v, "audio/mp4", 9) || !strncasecmp(v, "audio/x-m4a", 11) ||
			 !strncasecmp(v, "audio/m4a", 9) || !strncasecmp(v, "video/mp4", 9))
			codec = CODEC_MP4;
	} else if (!strcasecmp(line, "icy-metaint")) {
		metaint = atol(v);
		audio_left = metaint;
		finite = false;
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
#define PREBUFFER	(32 * 1024)	/* ~2 s at 128 kbit/s, if it doesn't say */
#define PREBUFFER_MIN	(8 * 1024)
#define PREBUFFER_MAX	(48 * 1024)

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
	return stop || restart_xfer ? 0 : len;
}

static int on_progress(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	(void)u; (void)a; (void)b; (void)c; (void)d;
	return stop || restart_xfer;	/* non-zero aborts the transfer */
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
	if (start_byte > 0)
		curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)start_byte);
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
			fprintf(stderr, "octopus-stream: fMP4 HLS isn't supported\n");
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
	size_t prebuf;
	struct timespec ts;
	pthread_t t;
	size_t n;

	ring_head = ring_len = 0;
	net_done = false;
	icy_br = 0;
	if (pthread_create(&t, NULL, net_thread, c))
		return CURLE_FAILED_INIT;
	while (!stop && !restart_xfer) {
		/* ~2 s of it: a 56k station shouldn't wait 7 s. */
		prebuf = icy_br > 0 ? (size_t)icy_br * 250 : PREBUFFER;
		prebuf = prebuf < PREBUFFER_MIN ? PREBUFFER_MIN : prebuf > PREBUFFER_MAX ? PREBUFFER_MAX : prebuf;
		pthread_mutex_lock(&ring_lock);
		while (!stop && !net_done && ring_len < (filled ? 1 : prebuf)) {
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
		snprintf(cur_target, sizeof(cur_target), "%s", target);
		restart_xfer = false;
		if (m4.indexed) {
			/* An MP4 we have the index of: straight to a frame. */
			codec = CODEC_MP4;
			m4.state = BOX_MDAT;
			m4.remain = -1;
			m4.part_have = 0;
			m4.dec_ready = false;
			if (m4.want_byte > 0) {
				start_byte = m4.want_byte;
				m4.want_byte = 0;
			}
			m4.next = mp4_frame_at(start_byte);
			if (m4.next < m4.n)
				start_byte = m4.off[m4.next];
		} else if (start_byte > 0 && looks_mp4(target)) {
			/* Resuming into an MP4: its index first, from the top. */
			m4.want_byte = start_byte;
			start_byte = 0;
		}
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
		if (restart_xfer)	/* an MP4's index read: now where we resume */
			continue;
		if (finite && file_len > 0 && base_byte + fed >= file_len) {
			set_state("ended");	/* the whole file: done */
			break;
		}
		if (finite)	/* dropped mid-file: carry on from there */
			start_byte = base_byte + fed;
failed:
		if (!strcmp(state, "playing"))
			backoff = 2;	/* it was working: retry soon */
		fprintf(stderr, "octopus-stream: %s: %s\n", target, curl_easy_strerror(res));
		set_state("error");
		sleep_s(backoff);
		backoff = backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff * 2;
	}
}

int main(int argc, char **argv)
{
	const char *dir = octopus_run_dir();
	char *audio_argv[] = { "octopus-audio", "play", "stream", NULL };
	bool resuming = false;
	struct sigaction sa;
	pid_t pid;
	FILE *f;

	snprintf(status_path, sizeof(status_path), "%s/stream", dir);
	snprintf(pid_path, sizeof(pid_path), "%s/stream.pid", dir);
	snprintf(paused_path, sizeof(paused_path), "%s/stream.paused", dir);
	snprintf(lock_path, sizeof(lock_path), "%s/stream.lock", dir);
	if (argc < 2) {
		fprintf(stderr, "usage: octopus-stream <url> [name] | stop | pause | resume | active\n");
		return 2;
	}
	if (!strcmp(argv[1], "active"))
		return running_pid() ? 0 : 1;
	/* One at a time from here to the new player's pid in place (it
	 * inherits the lock, and lets go once it's written: see below). */
	mkdir(dir, 0755);
	{
		int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);

		if (lock >= 0)
			flock(lock, LOCK_EX);
	}
	if (!strcmp(argv[1], "pause")) {
		char line[1200];
		FILE *st;

		if (!running_pid())
			return 0;
		/* What plays, to come back to: its URL and name (and, in a
		 * file, where). */
		st = fopen(status_path, "r");
		f = fopen(paused_path, "w");
		while (st && f && fgets(line, sizeof(line), st))
			if (!strncmp(line, "url=", 4) || !strncmp(line, "name=", 5) ||
			    !strncmp(line, "byte=", 5))
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
			} else if (!strncmp(line, "byte=", 5)) {
				start_byte = atoll(line + 5);
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
		if (argc > 3)
			start_byte = atoll(argv[3]);
		unlink(paused_path);
		/* Whatever else plays (the FM radio) stops: one at a time. A
		 * resume comes from octopus-audio itself (it holds the lock). */
		pid = fork();
		if (pid == 0) {
			execv(AUDIO, audio_argv);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}

	run_start = start_byte;
	/* Into the background, on our own (closing the app leaves us on). */
	pid = fork();
	if (pid < 0)
		return 1;
	if (pid > 0)
		return 0;
	setsid();
	setpriority(PRIO_PROCESS, 0, -10);
	/* We're the player now: the next start (or stop) finds us. */
	f = fopen(pid_path, "w");
	if (f) {
		fprintf(f, "%d\n", getpid());
		fclose(f);
	}
	/* Let go of what we were started with: a terminal, octopus-audio's
	 * lock (it would stay held as long as we play), our own lock (the
	 * next start may go ahead). stderr stays (the journal, from the
	 * apps). */
	{
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
		}
		for (fd = 3; fd < 256; fd++)
			close(fd);
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
	/* Replaced (killed after the 5 s, our pid file another's): the files
	 * and octopus-audio's "playing" are the new player's now. */
	if (!pid_is_ours())
		return 0;
	if (strcmp(state, "ended"))	/* that one stays, for the app */
		unlink(status_path);
	/* Not playing any more (stopped, paused, replaced): octopus-audio's
	 * "playing" goes, unless something else plays by now. Before we're
	 * gone - the stream replacing us waits for that - so it can't clear
	 * the new one's. */
	{
		char *stopped_argv[] = { "octopus-audio", "stopped", "stream", NULL };

		pid = fork();
		if (pid == 0) {
			execv(AUDIO, stopped_argv);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}
	unlink(pid_path);
	return 0;
}
