// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-stream: play an internet radio stream.
 *
 *   chumby-stream <url> [name]   start playing (replacing any stream, and
 *                                stopping the FM radio), in the background
 *   chumby-stream stop
 *
 * MP3 over http or https (libcurl), decoded by libmpg123 to the codec
 * (ALSA, as the FM loop uses it). .pls and .m3u playlists are followed to
 * their first entry. Shoutcast/Icecast "ICY" metadata gives what's
 * playing. A dropped stream is reconnected, backing off up to 30 s.
 *
 * State goes to $run_dir/stream for the app ("state=", "url=", "name=",
 * "title=" lines), gone once stopped; our pid to $run_dir/stream.pid.
 */

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <mpg123.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <chumby.h>

#define PCM		"plughw:0,0"
#define CA_BUNDLE	"/etc/ssl/certs/ca-certificates.crt"	/* mbed TLS: no dirs */
#define RADIO		"/usr/bin/radio"
#define RADIO_PIDFILE	"/var/run/radio-loop.pid"
#define USER_AGENT	"chumby-stream/1.0"
#define BACKOFF_MAX_S	30

static volatile sig_atomic_t stop;
static char status_path[256], pid_path[256];
static char url[1024], name[128], title[256], state[16];

static mpg123_handle *mh;
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

static void status_write(void)
{
	char tmp[300];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", status_path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "state=%s\nurl=%s\nname=%s\ntitle=%s\n", state, url, name, title);
	fclose(f);
	rename(tmp, status_path);
}

static void set_state(const char *s)
{
	if (strcmp(state, s)) {
		snprintf(state, sizeof(state), "%s", s);
		status_write();
	}
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

static bool fm_running(void)
{
	FILE *f = fopen(RADIO_PIDFILE, "r");
	int pid = 0;

	if (!f)
		return false;
	if (fscanf(f, "%d", &pid) != 1)
		pid = 0;
	fclose(f);
	return pid > 0 && kill(pid, 0) == 0;
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
	snprintf(title, sizeof(title), "%.*s", (int)(e - p), p);
	status_write();
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
	if (!strcasecmp(line, "icy-metaint")) {
		metaint = atol(v);
		audio_left = metaint;
	} else if (!strcasecmp(line, "icy-name") && !got_name && *v) {
		snprintf(name, sizeof(name), "%s", v);
		status_write();
	}
	return len;
}

static size_t on_data(char *p, size_t size, size_t n, void *user)
{
	size_t len = size * n, i = 0, take;

	(void)user;
	while (i < len && !stop) {
		if (!metaint) {
			decode(p + i, len - i);
			break;
		}
		if (meta_len < 0 && audio_left > 0) {		/* audio */
			take = len - i < (size_t)audio_left ? len - i : (size_t)audio_left;
			decode(p + i, take);
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

static bool is_playlist(const char *u)
{
	const char *q = strpbrk(u, "?#"), *end = q ? q : u + strlen(u);

	return (end - u > 4 && !strncasecmp(end - 4, ".pls", 4)) ||
	       (end - u > 4 && !strncasecmp(end - 4, ".m3u", 4)) ||
	       (end - u > 5 && !strncasecmp(end - 5, ".m3u8", 5));
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

static void sleep_s(int s)
{
	int i;

	for (i = 0; i < s * 10 && !stop; i++)
		usleep(100 * 1000);
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
		decoder_reset();
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
			res = curl_easy_perform(c);
			curl_slist_free_all(h);
		}
		curl_easy_cleanup(c);
		if (stop)
			break;
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
	char *radio_argv[] = { "radio", "off", NULL };
	struct sigaction sa;
	pid_t pid;
	FILE *f;

	snprintf(status_path, sizeof(status_path), "%s/stream", dir);
	snprintf(pid_path, sizeof(pid_path), "%s/stream.pid", dir);
	if (argc < 2) {
		fprintf(stderr, "usage: chumby-stream <url> [name] | stop\n");
		return 2;
	}
	stop_running();
	if (!strcmp(argv[1], "stop")) {
		unlink(status_path);
		return 0;
	}
	snprintf(url, sizeof(url), "%s", argv[1]);
	if (argc > 2 && argv[2][0]) {
		snprintf(name, sizeof(name), "%s", argv[2]);
		got_name = true;
	}

	/* Into the background, on our own (closing the app leaves us on). */
	pid = fork();
	if (pid < 0)
		return 1;
	if (pid > 0)
		return 0;
	setsid();
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

	/* The FM loop holds the sound card. */
	if (fm_running()) {
		pid = fork();
		if (pid == 0) {
			execv(RADIO, radio_argv);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}

	curl_global_init(CURL_GLOBAL_DEFAULT);
	mpg123_init();
	play();

	pcm_close();
	if (mh)
		mpg123_delete(mh);
	unlink(status_path);
	unlink(pid_path);
	return 0;
}
