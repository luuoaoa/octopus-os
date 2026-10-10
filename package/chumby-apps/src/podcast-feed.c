// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-podcast-feed: a podcast's RSS feed, its newest episodes cached.
 *
 *   chumby-podcast-feed <feed URL> <cache file> [max episodes]
 *
 * Feeds can be megabytes (a long-running show lists every episode ever,
 * newest first): it's parsed as it downloads (expat), and the download
 * stops once the newest ones are in (30 by default). The cache, written
 * whole (renamed into place) for the Podcasts app, is one line per
 * episode, tab-separated:
 *
 *   <published, epoch s> <duration s> <bytes> <audio URL> <title>
 *
 * Exit status 0 if any episodes were found.
 */

#define _GNU_SOURCE
#include <curl/curl.h>
#include <expat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define CA_BUNDLE	"/etc/ssl/certs/ca-certificates.crt"	/* mbed TLS: no dirs */
#define USER_AGENT	"chumby-podcasts/1.0"
#define MAX_DEFAULT	30

struct episode {
	char title[256];
	char url[1024];
	long long bytes;
	long duration;
	long long published;
};

static struct episode ep;		/* the <item> being read */
static bool in_item, in_text;
static char text[1024];			/* the element's text, so far */
static size_t text_len;
static int found, max_items;
static FILE *out;
static XML_Parser parser;
static bool done;			/* enough: stop downloading */

/* "1803", "25:17", "01:02:05" -> seconds. */
static long duration_s(const char *s)
{
	long v = 0, part = 0;

	for (; *s; s++) {
		if (*s >= '0' && *s <= '9') {
			part = part * 10 + (*s - '0');
		} else if (*s == ':') {
			v = (v + part) * 60;
			part = 0;
		} else if (*s != ' ') {
			break;
		}
	}
	return v + part;
}

/* RFC 822 ("Wed, 07 Oct 2026 07:00:00 +0000"), the zone ignored: days
 * are what the app shows. */
static long long pub_time(const char *s)
{
	struct tm tm;
	const char *p = strchr(s, ',');

	memset(&tm, 0, sizeof(tm));
	p = p ? p + 1 : s;
	while (*p == ' ')
		p++;
	if (!strptime(p, "%d %b %Y %H:%M:%S", &tm) && !strptime(p, "%d %b %Y", &tm))
		return 0;
	return (long long)timegm(&tm);
}

/* Tabs and newlines would break the line format. */
static void put_clean(const char *s)
{
	for (; *s; s++)
		fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, out);
}

static const char *attr(const XML_Char **atts, const char *name)
{
	for (; atts && atts[0]; atts += 2)
		if (!strcmp(atts[0], name))
			return atts[1];
	return NULL;
}

static void on_start(void *user, const XML_Char *el, const XML_Char **atts)
{
	(void)user;
	if (!strcmp(el, "item")) {
		memset(&ep, 0, sizeof(ep));
		in_item = true;
		return;
	}
	if (!in_item)
		return;
	if (!strcmp(el, "enclosure")) {
		const char *u = attr(atts, "url"), *len = attr(atts, "length"), *type = attr(atts, "type");

		/* Audio only (some shows add video, or a transcript). */
		if (u && (!type || !strncasecmp(type, "audio/", 6)) && !ep.url[0]) {
			snprintf(ep.url, sizeof(ep.url), "%s", u);
			ep.bytes = len ? atoll(len) : 0;
		}
		return;
	}
	if (!strcmp(el, "title") || !strcmp(el, "pubDate") || !strcmp(el, "itunes:duration")) {
		in_text = true;
		text_len = 0;
		text[0] = '\0';
	}
}

static void on_text(void *user, const XML_Char *s, int len)
{
	(void)user;
	if (!in_text)
		return;
	if (text_len + len >= sizeof(text))
		len = sizeof(text) - 1 - text_len;
	memcpy(text + text_len, s, len);
	text_len += len;
	text[text_len] = '\0';
}

static void on_end(void *user, const XML_Char *el)
{
	(void)user;
	if (in_text) {
		in_text = false;
		if (!strcmp(el, "title") && !ep.title[0])
			snprintf(ep.title, sizeof(ep.title), "%s", text);
		else if (!strcmp(el, "pubDate"))
			ep.published = pub_time(text);
		else if (!strcmp(el, "itunes:duration"))
			ep.duration = duration_s(text);
		return;
	}
	if (!strcmp(el, "item") && in_item) {
		in_item = false;
		if (!ep.url[0])
			return;
		fprintf(out, "%lld\t%ld\t%lld\t", ep.published, ep.duration, ep.bytes);
		put_clean(ep.url);
		fputc('\t', out);
		put_clean(ep.title[0] ? ep.title : "Episode");
		fputc('\n', out);
		if (++found >= max_items) {
			done = true;
			XML_StopParser(parser, XML_FALSE);
		}
	}
}

static size_t on_data(char *p, size_t size, size_t n, void *user)
{
	(void)user;
	if (done)
		return 0;	/* stops the download */
	if (XML_Parse(parser, p, (int)(size * n), 0) == XML_STATUS_ERROR && !done) {
		fprintf(stderr, "chumby-podcast-feed: %s\n", XML_ErrorString(XML_GetErrorCode(parser)));
		return 0;
	}
	return done ? 0 : size * n;
}

int main(int argc, char **argv)
{
	char tmp[1100];
	CURLcode res;
	CURL *c;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <feed URL> <cache file> [max episodes]\n", argv[0]);
		return 2;
	}
	max_items = argc > 3 && atoi(argv[3]) > 0 ? atoi(argv[3]) : MAX_DEFAULT;
	snprintf(tmp, sizeof(tmp), "%s.tmp", argv[2]);
	out = fopen(tmp, "w");
	if (!out) {
		perror(tmp);
		return 1;
	}
	parser = XML_ParserCreate(NULL);
	XML_SetElementHandler(parser, on_start, on_end);
	XML_SetCharacterDataHandler(parser, on_text);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_URL, argv[1]);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");	/* gzip: much smaller */
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	res = curl_easy_perform(c);
	if (!done && res == CURLE_OK)
		XML_Parse(parser, "", 0, 1);
	curl_easy_cleanup(c);
	XML_ParserFree(parser);
	fclose(out);

	if (!found) {
		fprintf(stderr, "chumby-podcast-feed: %s: no episodes (%s)\n", argv[1],
			res == CURLE_OK || done ? "none in the feed" : curl_easy_strerror(res));
		remove(tmp);
		return 1;
	}
	if (rename(tmp, argv[2]) < 0) {
		perror(argv[2]);
		return 1;
	}
	return 0;
}
