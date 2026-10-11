// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-podcasts: podcast episodes, laid out like Internet Radio:
 *
 *   left        the show's artwork
 *   right       the show's name, small, level with the art's top; under it
 *               the episode's title (scrolling twice when it changes, then
 *               cut with dots); its date (or how playing is going)
 *   under both  the progress bar, the screen's width (seek on it), with
 *               where in it at its left end and how long at its right
 *   bottom      episode list, back 10 s, play/pause, ahead 30 s
 *
 * Shows: "name | RSS feed URL | artwork URL" lines in /boot/podcasts.conf
 * (the config partition), else the defaults in /usr/share/chumby. Their
 * feeds are read by chumby-podcast-feed (the newest 30 episodes) into
 * /var/lib/chumby/podcasts, again once an hour old, one at a time, in the
 * background. Artwork is fetched once into /var/lib/chumby/art.
 *
 * chumby-stream plays (in the background: it carries on when this app
 * closes, and takes the sound from other sources through chumby-audio),
 * starting an episode where it was left: its status gives the byte it's
 * at, kept per episode in /var/lib/chumby/podcasts/positions, with the
 * finished ones marked. Pause stops it, remembering where; the skip
 * buttons restart it a little before or after (taps in a row add up).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <chumby.h>

#define CONF_USER	"/boot/podcasts.conf"
#define CONF_DEFAULT	"/usr/share/chumby/podcasts.conf"
#define DATA_DIR	"/var/lib/chumby/podcasts"
#define POSITIONS_FILE	DATA_DIR "/positions"
#define CURRENT_FILE	DATA_DIR "/current"
#define ART_DIR		"/var/lib/chumby/art"
#define STREAM_TOOL	"/usr/bin/chumby-stream"
#define FEED_TOOL	"/usr/bin/chumby-podcast-feed"
#define CURL		"/usr/bin/curl"
#define CA_BUNDLE	"/etc/ssl/certs/ca-certificates.crt"
#define MAX_SHOWS	16
#define MAX_EPS		30
#define MAX_POSITIONS	256
#define FEED_MAX_AGE_S	3600
#define ART_X		14
#define ART_SIZE	92
#define ART_PAD		8
#define COL_X		(ART_X + ART_SIZE + 12)
#define COL_W		(CHUMBY_APP_W - 10 - COL_X)
#define SMALL_BTN	38
#define LINE_GAP	2
#define BAR_H		4
#define BAR_W		(CHUMBY_APP_W - 2 * ART_X)	/* the width: seeking precisely */
#define BAR_GAP		8			/* between the art and the bar */
#define BAR_TOUCH	12			/* the bar's touch area, above and below */
#define STRIPE_W	28			/* the busy stripe on the bar */
#define STRIPE_MS	1100			/* its trip across */
#define START_GRACE_MS	5000
#define SAVE_EVERY_S	10
#define SKIP_SETTLE_MS	500
#define TILE_H		44
#define TILE_GAP	6
#define ROW_H		54
#define LIST_HEAD_H	30

struct episode {
	long long published;	/* epoch s */
	long duration;		/* s; 0: unknown */
	long long bytes;	/* 0: unknown */
	char url[512];
	char title[160];
};

struct show {
	char name[64];
	char feed[512];
	char art[512];
	bool art_tried;
	struct episode eps[MAX_EPS];
	int n_eps;
};

struct position {
	unsigned long key;	/* hash of the episode's URL */
	long long byte;
	long long length;
	bool done;
};

static struct show shows[MAX_SHOWS];
static int n_shows;
static struct position positions[MAX_POSITIONS];
static int n_positions;

static int cur_show, cur_ep = -1;	/* what's selected */
static bool playing;			/* we want it on (corrected by status) */
static uint64_t start_grace_until;
static long long st_byte, st_length;	/* where chumby-stream is, in the episode */
/* Where playing is, in seconds: where it started (the byte it started
 * from, at its bitrate: an estimate, made once) plus what chumby-stream
 * has actually played since ("played=", exact). It only moves on with
 * the sound, never back. (st_byte is what's been fed to the decoder: a
 * few seconds ahead, in network-sized steps.) */
static double play_anchor_s = -1;	/* seconds at played=0; -1: not known */
static long st_played = -1;		/* seconds played this run */
/* Runs: chumby-stream's "start=", the byte a run started at. Ours (a
 * play, a skip, a seek) is req_start; the one the status is about,
 * run_start. Another (a resume after an alarm) carries on from where
 * the time was. */
static long long req_start = -1, run_start = -1;
/* Bytes per second when the feed doesn't say: the stream's first
 * measured bitrate, kept for the episode (measurements wander). */
static double bps_kept;
static const struct episode *bps_kept_for;
static char st_state[16];
static int st_bitrate;
static time_t last_saved;
static long skip_s;			/* skips not applied yet */
static char message[96];

static int feed_child = -1, feed_show = -1, art_child = -1, art_show = -1, stream_child = -1;
static char art_tmp[300];

static lv_obj_t *scr, *lbl_show, *lbl_elapsed, *lbl_total, *lbl_title, *lbl_sub, *bar, *stripe;
static bool seeking;			/* a finger on the bar */
static lv_obj_t *art_tile, *art_img, *art_note;
static lv_obj_t *btn_list, *btn_back, *btn_play, *btn_ahead;
static lv_obj_t *list_panel;
static int list_level = -1;		/* open: -1 shows, else that show's episodes */
static lv_timer_t *skip_timer;

static uint64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

static unsigned long hash(const char *s)
{
	unsigned long h = 5381;

	for (; *s; s++)
		h = h * 33 + (unsigned char)*s;
	return h & 0xffffffffUL;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

static struct episode *episode(void)
{
	return n_shows && cur_ep >= 0 && cur_ep < shows[cur_show].n_eps ?
	       &shows[cur_show].eps[cur_ep] : NULL;
}

/* "1:02:05" or "25:17". */
static void fmt_time(char *out, size_t size, long s)
{
	if (s < 0)
		s = 0;
	if (s >= 3600)
		snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}

static void fmt_date(char *out, size_t size, long long t)
{
	time_t tt = (time_t)t;
	struct tm tm;

	if (t <= 0 || !localtime_r(&tt, &tm)) {
		out[0] = '\0';
		return;
	}
	strftime(out, size, "%b %e", &tm);
}

/* ------------------------------------------------------------------ */
/* Shows, their episodes, and where we are in them                    */

static void cache_path(int s, char *out, size_t size)
{
	snprintf(out, size, DATA_DIR "/%08lx.feed", hash(shows[s].feed));
}

static void load_shows(void)
{
	FILE *f = fopen(CONF_USER, "r");
	char line[1200], *a, *b, *c;

	if (!f)
		f = fopen(CONF_DEFAULT, "r");
	n_shows = 0;
	while (f && n_shows < MAX_SHOWS && fgets(line, sizeof(line), f)) {
		struct show *sh = &shows[n_shows];

		a = trim(line);
		if (!*a || *a == '#')
			continue;
		b = strchr(a, '|');
		if (!b)
			continue;
		*b++ = '\0';
		c = strchr(b, '|');
		if (c)
			*c++ = '\0';
		memset(sh, 0, sizeof(*sh));
		snprintf(sh->name, sizeof(sh->name), "%s", trim(a));
		snprintf(sh->feed, sizeof(sh->feed), "%s", trim(b));
		if (c)
			snprintf(sh->art, sizeof(sh->art), "%s", trim(c));
		if (sh->feed[0])
			n_shows++;
	}
	if (f)
		fclose(f);
}

/* chumby-podcast-feed's cache: published, duration, bytes, URL, title. */
static void load_episodes(int s)
{
	struct show *sh = &shows[s];
	char path[256], line[1024], *f[5], *p;
	FILE *fp;
	int i;

	cache_path(s, path, sizeof(path));
	fp = fopen(path, "r");
	sh->n_eps = 0;
	while (fp && sh->n_eps < MAX_EPS && fgets(line, sizeof(line), fp)) {
		struct episode *e = &sh->eps[sh->n_eps];

		line[strcspn(line, "\n")] = '\0';
		for (i = 0, p = line; i < 5 && p; i++) {
			f[i] = p;
			p = i < 4 ? strchr(p, '\t') : NULL;
			if (p)
				*p++ = '\0';
		}
		if (i < 5)
			continue;
		e->published = atoll(f[0]);
		e->duration = atol(f[1]);
		e->bytes = atoll(f[2]);
		snprintf(e->url, sizeof(e->url), "%s", f[3]);
		snprintf(e->title, sizeof(e->title), "%s", f[4]);
		sh->n_eps++;
	}
	if (fp)
		fclose(fp);
}

static struct position *position_of(const char *url, bool make)
{
	unsigned long key = hash(url);
	int i;

	for (i = 0; i < n_positions; i++)
		if (positions[i].key == key)
			return &positions[i];
	if (!make)
		return NULL;
	if (n_positions == MAX_POSITIONS) {	/* the oldest goes */
		memmove(positions, positions + 1, sizeof(positions[0]) * (MAX_POSITIONS - 1));
		n_positions--;
	}
	memset(&positions[n_positions], 0, sizeof(positions[0]));
	positions[n_positions].key = key;
	return &positions[n_positions++];
}

static void load_positions(void)
{
	FILE *f = fopen(POSITIONS_FILE, "r");
	struct position p;
	int done;

	n_positions = 0;
	while (f && n_positions < MAX_POSITIONS &&
	       fscanf(f, "%lx %lld %lld %d", &p.key, &p.byte, &p.length, &done) == 4) {
		p.done = done;
		positions[n_positions++] = p;
	}
	if (f)
		fclose(f);
}

static void save_positions(void)
{
	FILE *f;
	int i;

	mkdir(DATA_DIR, 0755);
	f = fopen(POSITIONS_FILE ".tmp", "w");
	if (!f)
		return;
	for (i = 0; i < n_positions; i++)
		fprintf(f, "%08lx %lld %lld %d\n", positions[i].key, positions[i].byte,
			positions[i].length, positions[i].done);
	fclose(f);
	rename(POSITIONS_FILE ".tmp", POSITIONS_FILE);
}

/* The episode's bytes per second: from the feed (its size over its
 * duration: both of the episode as published, so their ratio is the
 * bitrate even when what's served is longer - see episode_duration()),
 * else the stream's measured bitrate, else 128 kbit/s. */
static double bytes_per_s(const struct episode *e)
{
	if (e && e->bytes > 0 && e->duration > 0)
		return (double)e->bytes / e->duration;
	if (e && bps_kept_for == e)
		return bps_kept;
	if (e && st_bitrate > 0 && playing) {
		bps_kept = st_bitrate * 125.0;
		bps_kept_for = e;
		return bps_kept;
	}
	return 16000.0;
}

static long long episode_length(const struct episode *e)
{
	struct position *p = e ? position_of(e->url, false) : NULL;

	if (st_length > 0 && playing)
		return st_length;
	if (p && p->length > 0)
		return p->length;
	return e ? e->bytes : 0;
}

/* Seconds into the episode as heard (see play_anchor_s), or -1. */
static double heard_s(void)
{
	if (play_anchor_s < 0)
		return -1;
	return play_anchor_s + (st_played > 0 ? st_played : 0);
}

/* Where the episode is: playing, what's been heard (as a byte: for a
 * seek, or saving the place); else where it was left. */
static long long episode_byte(const struct episode *e)
{
	struct position *p;

	if (!e)
		return 0;
	if (playing && heard_s() >= 0)
		return (long long)(heard_s() * bytes_per_s(e));
	if (playing && st_byte > 0)
		return st_byte;
	p = position_of(e->url, false);
	return p && !p->done ? p->byte : 0;
}

static void save_current(void)
{
	struct episode *e = episode();
	FILE *f;

	if (!e)
		return;
	mkdir(DATA_DIR, 0755);
	f = fopen(CURRENT_FILE, "w");
	if (f) {
		fprintf(f, "%s\n%s\n", shows[cur_show].feed, e->url);
		fclose(f);
	}
}

static bool find_episode(const char *url, int *s_out, int *e_out)
{
	int s, e;

	for (s = 0; s < n_shows; s++)
		for (e = 0; e < shows[s].n_eps; e++)
			if (!strcmp(shows[s].eps[e].url, url)) {
				*s_out = s;
				*e_out = e;
				return true;
			}
	return false;
}

static void load_current(void)
{
	FILE *f = fopen(CURRENT_FILE, "r");
	char feed[512] = "", url[512] = "";
	int s;

	if (f) {
		if (fgets(feed, sizeof(feed), f))
			feed[strcspn(feed, "\n")] = '\0';
		if (fgets(url, sizeof(url), f))
			url[strcspn(url, "\n")] = '\0';
		fclose(f);
	}
	if (url[0] && find_episode(url, &cur_show, &cur_ep))
		return;
	cur_show = 0;
	for (s = 0; s < n_shows; s++)
		if (!strcmp(shows[s].feed, feed))
			cur_show = s;
	cur_ep = n_shows && shows[cur_show].n_eps ? 0 : -1;	/* the newest */
}

/* ------------------------------------------------------------------ */
/* Feeds and artwork, fetched in the background                       */

static void feed_next(void)
{
	char path[256], *argv[] = { "chumby-podcast-feed", NULL, path, "30", NULL };
	struct stat st;
	int s;

	if (feed_child > 0)
		return;
	for (s = 0; s < n_shows; s++) {
		cache_path(s, path, sizeof(path));
		if (stat(path, &st) == 0 && time(NULL) - st.st_mtime < FEED_MAX_AGE_S)
			continue;
		mkdir(DATA_DIR, 0755);
		argv[1] = shows[s].feed;
		feed_show = s;
		feed_child = chumby_spawn(FEED_TOOL, argv);
		/* Tried, worked or not: not again for an hour. */
		if (stat(path, &st) == 0) {
			struct timespec t[2] = { { .tv_nsec = UTIME_NOW }, { .tv_nsec = UTIME_NOW } };

			utimensat(AT_FDCWD, path, t, 0);
		}
		return;
	}
}

static void art_base(const struct show *sh, char *out, size_t size)
{
	snprintf(out, size, ART_DIR "/podcast-%08lx", hash(sh->art));
}

static bool art_path(const struct show *sh, char *out, size_t size)
{
	static const char *exts[] = { ".png", ".jpg" };
	char base[200];
	size_t i;

	if (!sh->art[0])
		return false;
	art_base(sh, base, sizeof(base));
	for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		snprintf(out, size, "%s%s", base, exts[i]);
		if (access(out, R_OK) == 0)
			return true;
	}
	return false;
}

static void art_fetch(int s)
{
	struct show *sh = &shows[s];
	char base[200];
	char *argv[] = { "curl", "-sfL", "-m", "20", "--cacert", CA_BUNDLE,
			 "-A", "chumby-podcasts/1.0", "-o", art_tmp, sh->art, NULL };

	if (art_child > 0 || sh->art_tried || !sh->art[0])
		return;
	sh->art_tried = true;
	mkdir(ART_DIR, 0755);
	art_base(sh, base, sizeof(base));
	snprintf(art_tmp, sizeof(art_tmp), "%s.tmp", base);
	art_show = s;
	art_child = chumby_spawn(CURL, argv);
}

/* Named by what it is: LVGL picks decoders by extension. */
static void art_fetched(bool ok)
{
	unsigned char magic[4] = { 0 };
	char base[200], path[220];
	const char *ext = NULL;
	FILE *f = ok ? fopen(art_tmp, "rb") : NULL;

	if (f) {
		if (fread(magic, 1, 4, f) == 4) {
			if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G')
				ext = ".png";
			else if (magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)
				ext = ".jpg";
		}
		fclose(f);
	}
	if (ext) {
		art_base(&shows[art_show], base, sizeof(base));
		snprintf(path, sizeof(path), "%s%s", base, ext);
		rename(art_tmp, path);
	} else {
		unlink(art_tmp);
	}
}

static void art_show_note(void)
{
	lv_obj_set_hidden(art_img, true);
	lv_obj_set_hidden(art_note, false);
	lv_obj_set_style_bg_color(art_tile, chumby_color(CHUMBY_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(art_note, chumby_on_accent(), 0);
	lv_obj_set_style_bg_grad_color(art_tile, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_VER, 0);
}

static void art_update(void)
{
	char path[220];

	if (!n_shows || !art_path(&shows[cur_show], path, sizeof(path)) ||
	    !chumby_logo_set(art_img, path, ART_SIZE)) {
		art_show_note();
		if (n_shows)
			art_fetch(cur_show);
		return;
	}
	/* Podcast art is a square picture: the whole tile. */
	lv_obj_center(art_img);
	lv_obj_set_hidden(art_img, false);
	lv_obj_set_hidden(art_note, true);
	lv_obj_set_style_bg_color(art_tile, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_bg_grad_dir(art_tile, LV_GRAD_DIR_NONE, 0);
}

/* ------------------------------------------------------------------ */
/* Playing                                                            */

static bool stream_is_ours(const char *url)
{
	struct episode *e = episode();

	return e && url[0] && !strcmp(url, e->url);
}

/*
 * How long it is: the file's real size (what the server says while it
 * plays, kept with its place) at its bitrate; the feed's word only until
 * that's known. Podcast hosts splice ads into the file as it's served
 * (seen: 12-15% longer than the feed's size and duration): by the feed's
 * duration, the time and bar reached the end with minutes still to play,
 * and a seek landed elsewhere than the bar said.
 */
static long episode_duration(const struct episode *e)
{
	long long len = episode_length(e);

	if (len > 0 && len != e->bytes)
		return (long)(len / bytes_per_s(e));
	return e->duration > 0 ? e->duration : (long)(len / bytes_per_s(e));
}

/* Busy (loading, or again after a seek): a stripe runs along the bar,
 * rather than words that don't fit. */
static void stripe_anim_cb(void *obj, int32_t x)
{
	lv_obj_set_x(obj, x);
}

static void busy_show(bool busy)
{
	lv_anim_t a;

	if (busy == !lv_obj_is_hidden(stripe))
		return;
	lv_obj_set_hidden(stripe, !busy);
	lv_anim_delete(stripe, stripe_anim_cb);
	if (!busy)
		return;
	lv_anim_init(&a);
	lv_anim_set_var(&a, stripe);
	lv_anim_set_exec_cb(&a, stripe_anim_cb);
	lv_anim_set_values(&a, -STRIPE_W, BAR_W);
	lv_anim_set_duration(&a, STRIPE_MS);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

static void label_set(lv_obj_t *l, const char *text)
{
	if (strcmp(lv_label_get_text(l), text))
		lv_label_set_text(l, text);
}

static void ui_update(void)
{
	struct episode *e = episode();
	char sub[96] = "", date[16], now[16] = "", total[16] = "";
	double bps = bytes_per_s(e);
	long long byte = episode_byte(e);
	long pos_s, dur_s;
	struct position *p = e ? position_of(e->url, false) : NULL;

	if (e) {
		pos_s = (long)(byte / bps) + skip_s;
		dur_s = episode_duration(e);
		if (pos_s > dur_s && dur_s > 0)
			pos_s = dur_s;
		fmt_time(now, sizeof(now), pos_s);
		fmt_time(total, sizeof(total), dur_s);
		lv_bar_set_value(bar, dur_s > 0 ? (int)(pos_s * 1000 / dur_s) : 0, LV_ANIM_OFF);
	} else {
		lv_bar_set_value(bar, 0, LV_ANIM_OFF);
	}
	fmt_date(date, sizeof(date), e ? e->published : 0);
	if (message[0])
		snprintf(sub, sizeof(sub), "%s", message);
	else if (playing && !strcmp(st_state, "error"))
		snprintf(sub, sizeof(sub), "can't reach it, retrying...");
	else if (e && p && p->done && !playing)
		snprintf(sub, sizeof(sub), "%s%splayed", date, date[0] ? "  " : "");
	else
		snprintf(sub, sizeof(sub), "%s", date);

	busy_show(playing && strcmp(st_state, "playing") && strcmp(st_state, "error") &&
		  !skip_s);
	/* Only what changed: a label set redraws, and twice a second adds up
	 * on the Chumby. */
	label_set(lbl_show, n_shows ? shows[cur_show].name : "No podcasts");
	if (strcmp(lv_label_get_text(lbl_elapsed), now))
		lv_label_set_text(lbl_elapsed, now);
	if (strcmp(lv_label_get_text(lbl_total), total))
		lv_label_set_text(lbl_total, total);
	chumby_scroll_label_set(lbl_title, e ? e->title : n_shows ? "Fetching episodes..." : "");
	if (strcmp(lv_label_get_text(lbl_sub), sub))
		lv_label_set_text(lbl_sub, sub);
	label_set(lv_obj_get_child(btn_play, 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	if (playing != lv_obj_has_state(btn_play, LV_STATE_CHECKED)) {
		if (playing)
			lv_obj_add_state(btn_play, LV_STATE_CHECKED);
		else
			lv_obj_remove_state(btn_play, LV_STATE_CHECKED);
	}
}

/* chumby-stream stops the old stream, then forks into the background:
 * reaped in poll_cb, so the UI never waits. */
static void stream_run(char **argv)
{
	int pid = chumby_spawn(STREAM_TOOL, argv);

	if (pid > 0)
		stream_child = pid;
}

static void position_save(const struct episode *e, long long byte, long long length, bool done)
{
	struct position *p;

	if (!e)
		return;
	p = position_of(e->url, true);
	p->byte = done ? 0 : byte;
	if (length > 0)
		p->length = length;
	p->done = done;
	save_positions();
	last_saved = time(NULL);
}

static void stream_start_at(long long byte)
{
	struct episode *e = episode();
	char at[24], *argv[] = { "chumby-stream", NULL, shows[cur_show].name, at, NULL };

	if (!e)
		return;
	if (byte < 0)
		byte = 0;
	snprintf(at, sizeof(at), "%lld", byte);
	argv[1] = e->url;
	stream_run(argv);
	start_grace_until = mono_ms() + START_GRACE_MS;
	snprintf(st_state, sizeof(st_state), "connecting");
	play_anchor_s = byte / bytes_per_s(e);
	st_played = 0;
	req_start = run_start = byte;
	st_byte = byte;
	st_length = 0;
	save_current();
}

static void stream_stop(void)
{
	char *argv[] = { "chumby-stream", "stop", NULL };

	stream_run(argv);
	start_grace_until = mono_ms() + START_GRACE_MS;
	st_state[0] = '\0';
}

static void pause_here(void)
{
	struct episode *e = episode();

	if (playing && e)
		position_save(e, episode_byte(e), st_length, false);
	playing = false;
	stream_stop();
	st_byte = st_length = 0;
	play_anchor_s = -1;
}

/* What chumby-stream says. */
static void status_read(void)
{
	char path[256], line[1200], state[16] = "", url[1024] = "";
	long long byte = 0, length = 0;
	int bitrate = 0;
	long played = -1;
	long long start = -1;
	struct episode *e;
	FILE *f;

	snprintf(path, sizeof(path), "%s/stream", chumby_run_dir());
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "state=", 6))
			snprintf(state, sizeof(state), "%.15s", line + 6);
		else if (!strncmp(line, "url=", 4))
			snprintf(url, sizeof(url), "%.1023s", line + 4);
		else if (!strncmp(line, "byte=", 5))
			byte = atoll(line + 5);
		else if (!strncmp(line, "length=", 7))
			length = atoll(line + 7);
		else if (!strncmp(line, "bitrate=", 8))
			bitrate = atoi(line + 8);
		else if (!strncmp(line, "played=", 7))
			played = atol(line + 7);
		else if (!strncmp(line, "start=", 6))
			start = atoll(line + 6);
	}
	if (f)
		fclose(f);
	/* Just started or stopped: not up to date yet (nothing, another's,
	 * or the last words of the run ours replaced). */
	if (mono_ms() < start_grace_until &&
	    (!f || !stream_is_ours(url) || (start >= 0 && start != req_start)))
		return;

	if (f && !strcmp(state, "ended") && stream_is_ours(url)) {
		/* To its end: done, and from the start if played again. */
		e = episode();
		if (playing || !position_of(e->url, false) || !position_of(e->url, false)->done)
			position_save(e, 0, length, true);
		playing = false;
		st_byte = st_length = 0;
		play_anchor_s = -1;
		unlink(path);
		ui_update();
		return;
	}
	if (!f || !stream_is_ours(url) || !strcmp(state, "ended")) {
		/* Not playing ours: stopped elsewhere (the volume control's
		 * stop, Net Radio, FM, an alarm). It played to where we last
		 * heard. */
		if (playing) {
			e = episode();
			position_save(e, episode_byte(e), st_length, false);
			playing = false;
			st_byte = st_length = 0;
			play_anchor_s = -1;
			ui_update();
		}
		return;
	}
	playing = true;		/* (or started elsewhere: resumed after an alarm) */
	snprintf(st_state, sizeof(st_state), "%s", state);
	if (byte > 0)
		st_byte = byte;
	if (length > 0)
		st_length = length;
	st_bitrate = bitrate;
	/* The bitrate known now (no size and length from the feed): our
	 * run's start, estimated with the 128k guess, estimated again. */
	e = episode();
	if (e && bps_kept_for != e && bitrate > 0 && !(e->bytes > 0 && e->duration > 0)) {
		(void)bytes_per_s(e);	/* keeps it */
		if (play_anchor_s >= 0 && req_start >= 0 && run_start == req_start)
			play_anchor_s = req_start / bytes_per_s(e);
	}
	if (played >= 0) {
		if (play_anchor_s < 0)		/* playing already (we just opened) */
			play_anchor_s = start >= 0 ? start / bytes_per_s(episode()) :
					st_byte / bytes_per_s(episode()) - played;
		else if (start != run_start)	/* a run not ours (resumed): carry on */
			play_anchor_s = heard_s() - played;
		run_start = start;
		st_played = played;
	}
	if (time(NULL) - last_saved >= SAVE_EVERY_S && st_byte > 0)
		position_save(episode(), episode_byte(episode()), st_length, false);
	if (!skip_s)
		ui_update();
}

static void list_refresh(void);

static void poll_cb(lv_timer_t *t)
{
	int status;

	(void)t;
	if (stream_child > 0 && waitpid(stream_child, &status, WNOHANG) == stream_child)
		stream_child = -1;
	if (art_child > 0 && waitpid(art_child, &status, WNOHANG) == art_child) {
		art_child = -1;
		art_fetched(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		if (art_show == cur_show)
			art_update();
	}
	if (feed_child > 0 && waitpid(feed_child, &status, WNOHANG) == feed_child) {
		char url[512] = "";

		feed_child = -1;
		if (episode())
			snprintf(url, sizeof(url), "%s", episode()->url);
		load_episodes(feed_show);
		/* The same episode, wherever the new list has it. */
		if (feed_show == cur_show && (!url[0] || !find_episode(url, &cur_show, &cur_ep)))
			cur_ep = shows[cur_show].n_eps ? 0 : -1;
		list_refresh();
		ui_update();
	}
	feed_next();
	status_read();
}

/* ------------------------------------------------------------------ */
/* Controls                                                           */

static void play_cb(lv_event_t *e)
{
	(void)e;
	if (!episode())
		return;
	message[0] = '\0';
	if (playing) {
		pause_here();
	} else {
		playing = true;
		stream_start_at(episode_byte(episode()));
	}
	ui_update();
}

/* The skips settled: from there. */
static void skip_timer_cb(lv_timer_t *t)
{
	struct episode *e = episode();
	long long byte;

	(void)t;
	lv_timer_pause(skip_timer);
	if (!e || !skip_s)
		return;
	byte = episode_byte(e) + (long long)(skip_s * bytes_per_s(e));
	skip_s = 0;
	if (byte < 0)
		byte = 0;
	if (episode_length(e) > 0 && byte >= episode_length(e))
		byte = episode_length(e) - (long long)(5 * bytes_per_s(e));
	if (playing)
		stream_start_at(byte);
	else
		position_save(e, byte, 0, false);
	ui_update();
}

static void skip_cb(lv_event_t *ev)
{
	if (!episode())
		return;
	skip_s += (long)(intptr_t)lv_event_get_user_data(ev);
	ui_update();
	lv_timer_reset(skip_timer);
	lv_timer_resume(skip_timer);
}

/* The bar: a finger on it picks where (the time follows it), letting go
 * goes there, as the skips do. */
static void bar_cb(lv_event_t *ev)
{
	lv_event_code_t code = lv_event_get_code(ev);
	struct episode *e = episode();
	lv_area_t a;
	lv_point_t pt;
	long dur_s, to_s;

	if (!e)
		return;
	if (code == LV_EVENT_PRESS_LOST) {	/* slid off: never mind */
		seeking = false;
		skip_s = 0;
		ui_update();
		return;
	}
	dur_s = episode_duration(e);
	if (dur_s <= 0)
		return;
	lv_obj_get_coords(bar, &a);
	lv_indev_get_point(lv_indev_active(), &pt);
	pt.x = LV_CLAMP(a.x1, pt.x, a.x2);
	to_s = (long)((long long)(pt.x - a.x1) * dur_s / lv_area_get_width(&a));
	/* As a skip from where it is (ui_update() shows that). */
	skip_s = to_s - (long)(episode_byte(e) / bytes_per_s(e));
	seeking = code != LV_EVENT_RELEASED;
	lv_timer_pause(skip_timer);
	if (seeking)
		ui_update();
	else
		skip_timer_cb(NULL);
}

/* ------------------------------------------------------------------ */
/* The list: shows as tiles, then a show's episodes as rows           */

static void list_show(int level);

static void list_close(void)
{
	if (list_panel)
		lv_obj_delete(list_panel);
	list_panel = NULL;
}

static void list_close_async(void *user)
{
	(void)user;
	list_close();
}

static void list_show_async(void *level)
{
	list_show((int)(intptr_t)level);
}

static void list_refresh(void)
{
	if (list_panel)
		list_show(list_level);
}

static void select_episode(int s, int ep)
{
	if (playing)
		pause_here();
	cur_show = s;
	cur_ep = ep;
	message[0] = '\0';
	art_update();
	playing = true;
	stream_start_at(episode_byte(episode()));
	ui_update();
}

static void episode_row_cb(lv_event_t *e)
{
	int ep = (int)(intptr_t)lv_event_get_user_data(e);

	if (list_level == cur_show && ep == cur_ep) {
		if (!playing)
			play_cb(NULL);
	} else {
		select_episode(list_level, ep);
	}
	lv_async_call(list_close_async, NULL);	/* not from inside its own event */
}

static void show_tile_cb(lv_event_t *e)
{
	lv_async_call(list_show_async, lv_event_get_user_data(e));
}

static void back_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(list_show_async, (void *)(intptr_t)-1);
}

static void close_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(list_close_async, NULL);
}

static lv_obj_t *tile(lv_obj_t *parent, const char *text, const char *sub, bool on)
{
	lv_obj_t *b = lv_button_create(parent), *l;

	lv_obj_set_size(b, (CHUMBY_APP_W - 3 * 8) / 2, TILE_H);
	lv_obj_set_style_radius(b, 8, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_pad_hor(b, 8, 0);
	lv_obj_set_style_bg_color(b, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_text_color(b, chumby_color(CHUMBY_COLOR_TEXT), 0);
	if (on) {
		chumby_glossy(b, chumby_color(CHUMBY_COLOR_ACCENT), 0);
		lv_obj_set_style_text_color(b, chumby_on_accent(), 0);
	}
	l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(l, (CHUMBY_APP_W - 3 * 8) / 2 - 16 - (sub ? 24 : 0));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_update_layout(l);
	if (lv_obj_get_height(l) > 2 * lv_font_get_line_height(chumby_font(CHUMBY_FONT_LARGE))) {
		lv_obj_set_height(l, 2 * lv_font_get_line_height(chumby_font(CHUMBY_FONT_LARGE)));
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	}
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
	if (sub) {
		l = lv_label_create(b);
		lv_label_set_text(l, sub);
		lv_obj_set_style_text_color(l, on ? chumby_on_accent() : chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
		lv_obj_align(l, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	return b;
}

/* An episode: its title, and its date, length and how far it's played. */
static lv_obj_t *episode_row(lv_obj_t *parent, int s, int ep)
{
	struct episode *e = &shows[s].eps[ep];
	struct position *p = position_of(e->url, false);
	bool on = s == cur_show && ep == cur_ep;
	lv_obj_t *b = lv_button_create(parent), *l;
	char date[16], info[64], left[16];
	double bps = e->bytes > 0 && e->duration > 0 ? (double)e->bytes / e->duration : 16000.0;

	lv_obj_set_size(b, CHUMBY_APP_W - 16, ROW_H);
	lv_obj_set_style_radius(b, 8, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_pad_hor(b, 10, 0);
	lv_obj_set_style_pad_ver(b, 0, 0);	/* the two lines need the height */
	lv_obj_set_style_bg_color(b, chumby_color(CHUMBY_COLOR_SURFACE), 0);
	lv_obj_set_style_text_color(b, chumby_color(CHUMBY_COLOR_TEXT), 0);
	if (on) {
		chumby_glossy(b, chumby_color(CHUMBY_COLOR_ACCENT), 0);
		lv_obj_set_style_text_color(b, chumby_on_accent(), 0);
	}
	l = lv_label_create(b);
	lv_label_set_text(l, e->title);
	lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(l, CHUMBY_APP_W - 16 - 20);
	lv_obj_set_height(l, lv_font_get_line_height(chumby_font(CHUMBY_FONT_LARGE)));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 4);

	fmt_date(date, sizeof(date), e->published);
	snprintf(info, sizeof(info), "%s", date);
	if (p && p->done) {
		snprintf(info + strlen(info), sizeof(info) - strlen(info), "  " LV_SYMBOL_OK " played");
	} else if (p && p->byte > 0 && e->duration > 0) {
		fmt_time(left, sizeof(left), e->duration - (long)(p->byte / bps));
		snprintf(info + strlen(info), sizeof(info) - strlen(info), "  %s left", left);
	} else if (e->duration > 0) {
		snprintf(info + strlen(info), sizeof(info) - strlen(info), "  %ld min",
			 (e->duration + 30) / 60);
	}
	l = lv_label_create(b);
	lv_label_set_text(l, info);
	lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_style_text_color(l, on ? chumby_on_accent() : chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, -4);
	return b;
}

/* level < 0: the shows; else that show's episodes. */
static void list_show(int level)
{
	lv_obj_t *head, *grid, *l, *b, *mark = NULL;
	int i;

	list_close();
	list_level = level;
	list_panel = lv_obj_create(scr);
	lv_obj_remove_style_all(list_panel);
	lv_obj_set_size(list_panel, CHUMBY_APP_W, CHUMBY_APP_H);
	lv_obj_set_style_bg_opa(list_panel, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(list_panel, chumby_color(CHUMBY_COLOR_BG), 0);
	lv_obj_set_scrollable(list_panel, false);

	head = lv_button_create(list_panel);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, CHUMBY_APP_W - 8 - CHUMBY_TOUCH_MIN, LIST_HEAD_H + 4);
	lv_obj_set_pos(head, 8, 0);
	l = lv_label_create(head);
	lv_obj_set_width(l, CHUMBY_APP_W - 8 - CHUMBY_TOUCH_MIN);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text_fmt(l, "%s%s", level < 0 ? "" : LV_SYMBOL_LEFT "  ",
			      level < 0 ? "Podcasts" : shows[level].name);
	lv_obj_set_style_text_font(l, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_style_text_color(l, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
	if (level >= 0)
		lv_obj_add_event_cb(head, back_cb, LV_EVENT_CLICKED, NULL);
	b = lv_button_create(list_panel);
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, CHUMBY_TOUCH_MIN, LIST_HEAD_H + 4);
	lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
	l = lv_label_create(b);
	lv_label_set_text(l, LV_SYMBOL_CLOSE);
	lv_obj_set_style_text_color(l, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, close_cb, LV_EVENT_CLICKED, NULL);

	grid = lv_obj_create(list_panel);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, CHUMBY_APP_W, CHUMBY_APP_H - LIST_HEAD_H - 4);
	lv_obj_set_pos(grid, 0, LIST_HEAD_H + 4);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_hor(grid, 8, 0);
	lv_obj_set_style_pad_bottom(grid, 8, 0);
	lv_obj_set_style_pad_gap(grid, TILE_GAP, 0);
	lv_obj_set_style_pad_column(grid, 8, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	if (level < 0) {
		for (i = 0; i < n_shows; i++) {
			b = tile(grid, shows[i].name, NULL, i == cur_show);
			lv_obj_add_event_cb(b, show_tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		}
		return;
	}
	if (!shows[level].n_eps) {
		l = lv_label_create(grid);
		lv_label_set_text(l, feed_child > 0 && feed_show == level ? "Fetching episodes..." :
				  "No episodes yet (no network?)");
		lv_obj_set_style_text_color(l, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
		return;
	}
	for (i = 0; i < shows[level].n_eps; i++) {
		b = episode_row(grid, level, i);
		lv_obj_add_event_cb(b, episode_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		if (level == cur_show && i == cur_ep)
			mark = b;
	}
	if (mark)
		lv_obj_scroll_to_view(mark, LV_ANIM_OFF);
}

static void list_cb(lv_event_t *e)
{
	(void)e;
	if (list_panel)
		list_close();
	else	/* straight into the selected show's episodes */
		list_show(n_shows > 1 ? cur_show : -1);
	if (!list_panel && n_shows == 1)
		list_show(0);
}

/* ------------------------------------------------------------------ */

static lv_obj_t *round_button(lv_obj_t *parent, const char *sym, int size,
			      const lv_font_t *font)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_center(l);
	return b;
}

static void on_mode(chumby_mode_t mode, void *user)
{
	lv_color_t surface = chumby_color(CHUMBY_COLOR_SURFACE);

	(void)user;
	(void)mode;
	lv_obj_set_style_text_color(lbl_show, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_elapsed, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_total, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_text_color(lbl_title, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(lbl_sub, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_bg_color(bar, chumby_color(CHUMBY_COLOR_SURFACE), LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, chumby_color(CHUMBY_COLOR_ACCENT), LV_PART_INDICATOR);
	/* Shows on the track and on the filled part alike. */
	lv_obj_set_style_bg_color(stripe, chumby_color(CHUMBY_COLOR_TEXT), 0);
	lv_obj_set_style_bg_color(btn_back, surface, 0);
	lv_obj_set_style_bg_color(btn_ahead, surface, 0);
	lv_obj_set_style_bg_color(btn_play, surface, 0);
	lv_obj_set_style_bg_color(btn_list, surface, LV_STATE_PRESSED);
	lv_obj_set_style_text_color(btn_list, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	lv_obj_set_style_border_color(btn_list, chumby_color(CHUMBY_COLOR_TEXT_DIM), 0);
	if (!lv_obj_is_hidden(art_note))
		art_show_note();
	if (list_panel)
		list_close();
}

int main(void)
{
	lv_obj_t *row;
	int32_t art_y, title_y, line_large, line_body;
	int s;

	scr = chumby_app_init("Podcasts");
	lv_obj_set_scrollable(scr, false);
	line_large = lv_font_get_line_height(chumby_font(CHUMBY_FONT_LARGE));
	line_body = lv_font_get_line_height(chumby_font(CHUMBY_FONT_BODY));

	/* Bottom: the episode list (small), back 10 s, play/pause, ahead 30 s. */
	row = lv_obj_create(scr);
	lv_obj_remove_style_all(row);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_size(row, CHUMBY_APP_W, 56);
	lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	btn_list = round_button(row, LV_SYMBOL_LIST, SMALL_BTN, &lv_font_montserrat_14);
	lv_obj_set_style_bg_opa(btn_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(btn_list, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(btn_list, 1, 0);
	lv_obj_set_style_border_opa(btn_list, LV_OPA_40, 0);
	lv_obj_set_ext_click_area(btn_list, (CHUMBY_TOUCH_MIN - SMALL_BTN) / 2);
	lv_obj_add_event_cb(btn_list, list_cb, LV_EVENT_CLICKED, NULL);
	btn_back = round_button(row, CHUMBY_ICON_BACK10, 52, chumby_icon_font(28));
	lv_obj_add_event_cb(btn_back, skip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
	btn_play = round_button(row, LV_SYMBOL_PLAY, 56, &lv_font_montserrat_28);
	lv_obj_add_event_cb(btn_play, play_cb, LV_EVENT_CLICKED, NULL);
	btn_ahead = round_button(row, CHUMBY_ICON_AHEAD30, 52, chumby_icon_font(28));
	lv_obj_add_event_cb(btn_ahead, skip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)30);
	lv_obj_update_layout(scr);

	/* The artwork, then the bar across the screen with the times under
	 * it: together, centred in the space above the buttons. */
	art_y = (lv_obj_get_y(row) - (ART_SIZE + BAR_GAP + BAR_H + LINE_GAP + line_body)) / 2;
	art_tile = lv_obj_create(scr);
	lv_obj_remove_style_all(art_tile);
	lv_obj_set_pos(art_tile, ART_X, art_y);
	lv_obj_set_size(art_tile, ART_SIZE, ART_SIZE);
	lv_obj_set_style_radius(art_tile, 10, 0);
	lv_obj_set_style_bg_opa(art_tile, LV_OPA_COVER, 0);
	lv_obj_set_style_clip_corner(art_tile, true, 0);
	lv_obj_set_clickable(art_tile, false);
	lv_obj_set_scrollable(art_tile, false);
	art_img = lv_image_create(art_tile);
	lv_obj_set_hidden(art_img, true);
	art_note = lv_label_create(art_tile);
	lv_label_set_text(art_note, CHUMBY_ICON_PODCAST);
	/* Its own size, not scaled up: a transform scales from the top left
	 * corner (off centre), and blurs. */
	lv_obj_set_style_text_font(art_note, chumby_icon_font(42), 0);
	lv_obj_center(art_note);

	/* Right column, level with the art's top: the show, small. */
	lbl_show = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_show, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_width(lbl_show, COL_W);
	lv_obj_set_height(lbl_show, line_body);
	lv_label_set_long_mode(lbl_show, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(lbl_show, COL_X, art_y);
	lv_label_set_text(lbl_show, "");
	/* Under them: the episode, its date (or how it's going). */
	title_y = art_y + line_body + LINE_GAP;
	lbl_title = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_title, chumby_font(CHUMBY_FONT_LARGE), 0);
	lv_obj_set_width(lbl_title, COL_W);
	chumby_scroll_label_init(lbl_title);
	lv_obj_set_pos(lbl_title, COL_X, title_y);
	lbl_sub = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_sub, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_width(lbl_sub, COL_W);
	lv_obj_set_height(lbl_sub, line_body);
	lv_label_set_long_mode(lbl_sub, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(lbl_sub, "");
	lv_obj_set_pos(lbl_sub, COL_X, title_y + line_large + LINE_GAP);
	/* The progress bar, the screen's width under the art (the wider, the
	 * finer a seek); where it's at under its left end, how long under its
	 * right. */
	bar = lv_bar_create(scr);
	lv_obj_set_size(bar, BAR_W, BAR_H);
	lv_obj_set_pos(bar, ART_X, art_y + ART_SIZE + BAR_GAP);
	lbl_elapsed = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_elapsed, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_label_set_text(lbl_elapsed, "");
	lv_obj_set_pos(lbl_elapsed, ART_X, art_y + ART_SIZE + BAR_GAP + BAR_H + LINE_GAP);
	lbl_total = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_total, chumby_font(CHUMBY_FONT_BODY), 0);
	lv_obj_set_width(lbl_total, BAR_W / 2);
	lv_obj_set_style_text_align(lbl_total, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_text(lbl_total, "");
	lv_obj_set_pos(lbl_total, ART_X + BAR_W / 2, art_y + ART_SIZE + BAR_GAP + BAR_H + LINE_GAP);
	lv_bar_set_range(bar, 0, 1000);
	lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_MAIN);
	lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
	/* Seekable: a finger's worth above and below the thin bar. */
	lv_obj_set_clickable(bar, true);
	lv_obj_set_ext_click_area(bar, BAR_TOUCH);
	lv_obj_add_event_cb(bar, bar_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(bar, bar_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(bar, bar_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(bar, bar_cb, LV_EVENT_PRESS_LOST, NULL);
	stripe = lv_obj_create(bar);
	lv_obj_remove_style_all(stripe);
	lv_obj_set_size(stripe, STRIPE_W, BAR_H);
	lv_obj_set_style_radius(stripe, BAR_H / 2, 0);
	lv_obj_set_style_bg_opa(stripe, LV_OPA_50, 0);
	lv_obj_set_clickable(stripe, false);
	lv_obj_set_hidden(stripe, true);

	skip_timer = lv_timer_create(skip_timer_cb, SKIP_SETTLE_MS, NULL);
	lv_timer_pause(skip_timer);

	load_shows();
	for (s = 0; s < n_shows; s++)
		load_episodes(s);
	load_positions();
	load_current();
	chumby_on_mode_change(on_mode, NULL);
	status_read();		/* already playing? which episode? */
	{
		char path[256], line[1200];
		FILE *f;

		/* Playing one of ours (resumed after a restart): show it. */
		snprintf(path, sizeof(path), "%s/stream", chumby_run_dir());
		f = fopen(path, "r");
		while (f && fgets(line, sizeof(line), f))
			if (!strncmp(line, "url=", 4)) {
				line[strcspn(line, "\n")] = '\0';
				if (find_episode(line + 4, &cur_show, &cur_ep))
					status_read();
			}
		if (f)
			fclose(f);
	}
	art_update();
	ui_update();
	feed_next();
	lv_timer_create(poll_cb, 500, NULL);

	return chumby_run();
}
