// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-airplay-remote: remote control of the iPhone, iPad or Mac that
 * plays to us over AirPlay (DACP, as the Apple Remote app did).
 *
 *   chumby-airplay-remote <command>
 *       nextitem, previtem, playpause, pause, play, stop, ...
 *
 * The sender says who it is when a session starts, and shairport-sync
 * passes that on in its metadata, which chumby-airplay-meta keeps in
 * $run_dir/airplay: "dacp_id=" (its DACP-ID), "active_remote=" (the token
 * a command must carry) and "client_ip=" (where it is). Its remote
 * control listens on a port it advertises over mDNS, as
 * iTunes_Ctrl_<DACP-ID>._dacp._tcp.local: we ask it directly (a unicast
 * query to its port 5353, which it answers to us), then send
 *
 *   GET /ctrl-int/1/<command> HTTP/1.1
 *   Active-Remote: <token>
 *
 * The port is kept in $run_dir/airplay-dacp for the next command (until
 * another sender, or the same one in a new session, comes along).
 * Exit status 0 if the sender took it, 1 if not (no session, nobody
 * answered), 2 for usage.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RUN_DEFAULT	"/run/chumby"
#define MDNS_PORT	5353
#define TIMEOUT_MS	2500

static char dacp_id[64], active_remote[64], client_ip[80];
static const char *run_dir;

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void session_read(void)
{
	char path[256], line[400];
	FILE *f;

	snprintf(path, sizeof(path), "%s/airplay", run_dir);
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "dacp_id=", 8))
			snprintf(dacp_id, sizeof(dacp_id), "%.63s", line + 8);
		else if (!strncmp(line, "active_remote=", 14))
			snprintf(active_remote, sizeof(active_remote), "%.63s", line + 14);
		else if (!strncmp(line, "client_ip=", 10))
			snprintf(client_ip, sizeof(client_ip), "%.79s", line + 10);
	}
	if (f)
		fclose(f);
}

/* An IPv6 link-local address needs its interface: the first one up that
 * has such an address too (there's the one Wi-Fi). */
static unsigned link_local_scope(void)
{
	struct ifaddrs *ifs, *i;
	unsigned scope = 0;

	if (getifaddrs(&ifs) < 0)
		return 0;
	for (i = ifs; i && !scope; i = i->ifa_next) {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)i->ifa_addr;

		if (a && a->sin6_family == AF_INET6 && (i->ifa_flags & IFF_UP) &&
		    !(i->ifa_flags & IFF_LOOPBACK) && IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr))
			scope = if_nametoindex(i->ifa_name);
	}
	freeifaddrs(ifs);
	return scope;
}

/* The sender's address, at a port. shairport-sync may give an IPv4
 * address mapped into IPv6 (::ffff:a.b.c.d): plain IPv4 then. */
static int sender_addr(int port, struct sockaddr_storage *ss, socklen_t *len)
{
	const char *ip = client_ip;
	char host[80];
	char *pct;

	if (!strncasecmp(ip, "::ffff:", 7) && strchr(ip + 7, '.'))
		ip += 7;
	snprintf(host, sizeof(host), "%s", ip);
	pct = strchr(host, '%');
	if (pct)
		*pct = '\0';
	memset(ss, 0, sizeof(*ss));
	if (strchr(host, ':')) {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)ss;

		if (inet_pton(AF_INET6, host, &a->sin6_addr) != 1)
			return -1;
		a->sin6_family = AF_INET6;
		a->sin6_port = htons(port);
		if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr))
			a->sin6_scope_id = pct ? if_nametoindex(pct + 1) : link_local_scope();
		*len = sizeof(*a);
	} else {
		struct sockaddr_in *a = (struct sockaddr_in *)ss;

		if (inet_pton(AF_INET, host, &a->sin_addr) != 1)
			return -1;
		a->sin_family = AF_INET;
		a->sin_port = htons(port);
		*len = sizeof(*a);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* The port: mDNS                                                     */

static int put_name(unsigned char *p, const char *name)
{
	int n = 0;
	const char *s = name;

	while (*s) {
		const char *dot = strchr(s, '.');
		int len = dot ? (int)(dot - s) : (int)strlen(s);

		p[n++] = len;
		memcpy(p + n, s, len);
		n += len;
		s += len + (dot ? 1 : 0);
	}
	p[n++] = 0;
	return n;
}

/* Past a (possibly compressed) name at p[i]; -1 if it runs off. */
static int skip_name(const unsigned char *p, int n, int i)
{
	while (i < n) {
		if (!p[i])
			return i + 1;
		if ((p[i] & 0xc0) == 0xc0)
			return i + 2 <= n ? i + 2 : -1;
		i += p[i] + 1;
	}
	return -1;
}

/* The port of an SRV record in an mDNS answer (any: we only asked for
 * one), or 0. */
static int srv_port(const unsigned char *p, int n)
{
	int qd, rr, i = 12, k;

	if (n < 12 || !(p[2] & 0x80))
		return 0;		/* not an answer */
	qd = p[4] << 8 | p[5];
	rr = (p[6] << 8 | p[7]) + (p[8] << 8 | p[9]) + (p[10] << 8 | p[11]);
	for (k = 0; k < qd && i >= 0; k++)
		i = (i = skip_name(p, n, i)) < 0 ? -1 : i + 4;
	for (k = 0; k < rr && i >= 0; k++) {
		int type, rdlen;

		i = skip_name(p, n, i);
		if (i < 0 || i + 10 > n)
			return 0;
		type = p[i] << 8 | p[i + 1];
		rdlen = p[i + 8] << 8 | p[i + 9];
		i += 10;
		if (i + rdlen > n)
			return 0;
		if (type == 33 && rdlen >= 6)	/* SRV: priority, weight, port */
			return p[i + 4] << 8 | p[i + 5];
		i += rdlen;
	}
	return 0;
}

static int port_lookup(void)
{
	unsigned char q[300] = { 0 }, buf[1500];
	struct sockaddr_storage to;
	socklen_t to_len;
	char name[160];
	long long until = mono_ms() + TIMEOUT_MS, next_send = 0;
	int s, len = 12, port = 0;

	if (sender_addr(MDNS_PORT, &to, &to_len) < 0)
		return 0;
	snprintf(name, sizeof(name), "iTunes_Ctrl_%s._dacp._tcp.local", dacp_id);
	q[5] = 1;			/* one question */
	len += put_name(q + len, name);
	q[len++] = 0;
	q[len++] = 33;			/* SRV */
	q[len++] = 0x80;		/* unicast response, please */
	q[len++] = 1;			/* IN */
	/* From a port of our own (not 5353): a "legacy" query, answered
	 * straight back to us. */
	s = socket(to.ss_family, SOCK_DGRAM, 0);
	if (s < 0)
		return 0;
	while (!port && mono_ms() < until) {
		struct pollfd pfd = { .fd = s, .events = POLLIN };
		int n;

		if (mono_ms() >= next_send) {	/* again, in case it's lost */
			sendto(s, q, len, 0, (struct sockaddr *)&to, to_len);
			next_send = mono_ms() + 700;
		}
		if (poll(&pfd, 1, 100) <= 0)
			continue;
		n = recv(s, buf, sizeof(buf), 0);
		if (n > 0)
			port = srv_port(buf, n);
	}
	close(s);
	return port;
}

/* The port, as found for this sender before (by its DACP-ID). */
static int port_cached(void)
{
	char path[256], id[64];
	int port = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/airplay-dacp", run_dir);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%63s %d", id, &port) != 2 || strcmp(id, dacp_id))
		port = 0;
	fclose(f);
	return port;
}

static void port_save(int port)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/airplay-dacp", run_dir);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s %d\n", dacp_id, port);
		fclose(f);
	}
}

static void port_forget(void)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/airplay-dacp", run_dir);
	unlink(path);
}

/* ------------------------------------------------------------------ */
/* The command: HTTP                                                  */

/* 1 if the sender took it, 0 if it said no, -1 if we never reached it. */
static int send_command(int port, const char *cmd)
{
	struct sockaddr_storage to;
	socklen_t to_len;
	char req[400], resp[64];
	struct pollfd pfd;
	int s, n, code, flags_ok;
	size_t got = 0;

	if (sender_addr(port, &to, &to_len) < 0)
		return -1;
	s = socket(to.ss_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (s < 0)
		return -1;
	if (connect(s, (struct sockaddr *)&to, to_len) < 0 && errno != EINPROGRESS) {
		close(s);
		return -1;
	}
	pfd.fd = s;
	pfd.events = POLLOUT;
	flags_ok = poll(&pfd, 1, TIMEOUT_MS) == 1 && !(pfd.revents & (POLLERR | POLLHUP));
	if (!flags_ok) {
		close(s);
		return -1;
	}
	n = snprintf(req, sizeof(req),
		     "GET /ctrl-int/1/%s HTTP/1.1\r\nHost: %s\r\nActive-Remote: %s\r\n"
		     "Connection: close\r\n\r\n", cmd, client_ip, active_remote);
	if (send(s, req, n, MSG_NOSIGNAL) != n) {
		close(s);
		return -1;
	}
	/* Just the status line: "HTTP/1.1 204 No Content". */
	pfd.events = POLLIN;
	while (got < sizeof(resp) - 1 && !memchr(resp, '\n', got) &&
	       poll(&pfd, 1, TIMEOUT_MS) == 1) {
		n = recv(s, resp + got, sizeof(resp) - 1 - got, 0);
		if (n <= 0)
			break;
		got += n;
	}
	close(s);
	resp[got] = '\0';
	if (sscanf(resp, "HTTP/%*s %d", &code) != 1)
		return -1;
	return code >= 200 && code < 300;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : NULL;
	int port, r;
	bool fresh = false;

	if (!cmd || !*cmd || strpbrk(cmd, " \r\n/?")) {
		fprintf(stderr, "usage: chumby-airplay-remote nextitem|previtem|playpause|pause|play|stop\n");
		return 2;
	}
	run_dir = getenv("CHUMBY_RUN_DIR") && *getenv("CHUMBY_RUN_DIR") ?
		  getenv("CHUMBY_RUN_DIR") : RUN_DEFAULT;
	session_read();
	if (!dacp_id[0] || !active_remote[0] || !client_ip[0]) {
		fprintf(stderr, "chumby-airplay-remote: no AirPlay session to control\n");
		return 1;
	}
	port = port_cached();
	if (!port) {
		port = port_lookup();
		fresh = true;
	}
	if (!port) {
		fprintf(stderr, "chumby-airplay-remote: %s doesn't answer (mDNS)\n", client_ip);
		return 1;
	}
	r = send_command(port, cmd);
	if (r < 0 && !fresh) {		/* it moved: look again */
		port = port_lookup();
		r = port ? send_command(port, cmd) : -1;
	}
	if (r < 0) {
		port_forget();
		fprintf(stderr, "chumby-airplay-remote: can't reach %s\n", client_ip);
		return 1;
	}
	port_save(port);
	return r ? 0 : 1;
}
