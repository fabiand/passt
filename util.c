// SPDX-License-Identifier: AGPL-3.0-or-later

/* PASST - Plug A Simple Socket Transport
 *  for qemu/UNIX domain socket mode
 *
 * PASTA - Pack A Subtle Tap Abstraction
 *  for network namespace/tap device mode
 *
 * util.c - Convenience helpers
 *
 * Copyright (c) 2020-2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#include <sched.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "util.h"
#include "passt.h"

/* For __openlog() and __setlogmask() wrappers, and passt_vsyslog() */
static int	log_mask;
static int	log_sock = -1;
static char	log_ident[BUFSIZ];
static int	log_opt;
static time_t	log_debug_start;

#define logfn(name, level)						\
void name(const char *format, ...) {					\
	struct timespec tp;						\
	va_list args;							\
									\
	if (setlogmask(0) & LOG_MASK(LOG_DEBUG)) {			\
		clock_gettime(CLOCK_REALTIME, &tp);			\
		fprintf(stderr, "%li.%04li: ",				\
			tp.tv_sec - log_debug_start,			\
			tp.tv_nsec / (100L * 1000));			\
	} else {							\
		va_start(args, format);					\
		passt_vsyslog(level, format, args);			\
		va_end(args);						\
	}								\
									\
	if (setlogmask(0) & LOG_MASK(LOG_DEBUG) ||			\
	    setlogmask(0) == LOG_MASK(LOG_EMERG)) {			\
		va_start(args, format);					\
		vfprintf(stderr, format, args); 			\
		va_end(args);						\
		if (format[strlen(format)] != '\n')			\
			fprintf(stderr, "\n");				\
	}								\
}

logfn(err,   LOG_ERR)
logfn(warn,  LOG_WARNING)
logfn(info,  LOG_INFO)
logfn(debug, LOG_DEBUG)

/**
 * __openlog() - Non-optional openlog() wrapper, to allow custom vsyslog()
 * @ident:	openlog() identity (program name)
 * @option:	openlog() options
 * @facility:	openlog() facility (LOG_DAEMON)
 */
void __openlog(const char *ident, int option, int facility)
{
	struct timespec tp;

	clock_gettime(CLOCK_REALTIME, &tp);
	log_debug_start = tp.tv_sec;

	if (log_sock < 0) {
		struct sockaddr_un a = { .sun_family = AF_UNIX, };

		log_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (log_sock < 0)
			return;

		strncpy(a.sun_path, _PATH_LOG, sizeof(a.sun_path));
		if (connect(log_sock, (const struct sockaddr *)&a, sizeof(a))) {
			close(log_sock);
			log_sock = -1;
			return;
		}
	}

	log_mask |= facility;
	strncpy(log_ident, ident, sizeof(log_ident) - 1);
	log_opt = option;

	openlog(ident, option, facility);
}

/**
 * __setlogmask() - setlogmask() wrapper, to allow custom vsyslog()
 * @mask:	Same as setlogmask() mask
 */
void __setlogmask(int mask)
{
	log_mask = mask;
	setlogmask(mask);
}

/**
 * passt_vsyslog() - vsyslog() implementation not using heap memory
 * @pri:	Facility and level map, same as priority for vsyslog()
 * @format:	Same as vsyslog() format
 * @ap:		Same as vsyslog() ap
 */
void passt_vsyslog(int pri, const char *format, va_list ap)
{
	char buf[BUFSIZ];
	int n;

	if (!(LOG_MASK(LOG_PRI(pri)) & log_mask))
		return;

	/* Send without name and timestamp, the system logger should add them */
	n = snprintf(buf, BUFSIZ, "<%i> ", pri);

	n += vsnprintf(buf + n, BUFSIZ - n, format, ap);

	if (format[strlen(format)] != '\n')
		n += snprintf(buf + n, BUFSIZ - n, "\n");

	if (log_opt & LOG_PERROR)
		fprintf(stderr, "%s", buf + sizeof("<0>"));

	send(log_sock, buf, n, 0);
}

/**
 * ipv6_l4hdr() - Find pointer to L4 header in IPv6 packet and extract protocol
 * @ip6h:	IPv6 header
 * @proto:	Filled with L4 protocol number
 *
 * Return: pointer to L4 header, NULL if not found
 */
char *ipv6_l4hdr(struct ipv6hdr *ip6h, uint8_t *proto)
{
	int offset, len, hdrlen;
	struct ipv6_opt_hdr *o;
	uint8_t nh;

	len = ntohs(ip6h->payload_len);
	offset = 0;

	while (offset < len) {
		if (!offset) {
			nh = ip6h->nexthdr;
			hdrlen = sizeof(struct ipv6hdr);
		} else {
			o = (struct ipv6_opt_hdr *)(((char *)ip6h) + offset);
			nh = o->nexthdr;
			hdrlen = (o->hdrlen + 1) * 8;
		}

		if (nh == 59)
			return NULL;

		if (nh == 0   || nh == 43  || nh == 44  || nh == 50  ||
		    nh == 51  || nh == 60  || nh == 135 || nh == 139 ||
		    nh == 140 || nh == 253 || nh == 254) {
			offset += hdrlen;
		} else {
			*proto = nh;
			return (char *)(ip6h + 1) + offset;
		}
	}

	return NULL;
}

/**
 * sock_l4() - Create and bind socket for given L4, add to epoll list
 * @c:		Execution context
 * @af:		Address family, AF_INET or AF_INET6
 * @proto:	Protocol number
 * @port:	Port, host order
 * @bind_type:	Type of address for binding
 * @data:	epoll reference portion for protocol handlers
 *
 * Return: newly created socket, -1 on error
 */
int sock_l4(struct ctx *c, int af, uint8_t proto, uint16_t port,
	    enum bind_type bind_addr, uint32_t data)
{
	union epoll_ref ref = { .r.proto = proto, .r.p.data = data };
	struct sockaddr_in addr4 = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		{ 0 }, { 0 },
	};
	struct sockaddr_in6 addr6 = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(port),
		0, IN6ADDR_ANY_INIT, 0,
	};
	const struct sockaddr *sa;
	struct epoll_event ev;
	int fd, sl, one = 1;

	if (proto != IPPROTO_TCP && proto != IPPROTO_UDP &&
	    proto != IPPROTO_ICMP && proto != IPPROTO_ICMPV6)
		return -1;	/* Not implemented. */

	if (proto == IPPROTO_TCP)
		fd = socket(af, SOCK_STREAM | SOCK_NONBLOCK, proto);
	else
		fd = socket(af, SOCK_DGRAM | SOCK_NONBLOCK, proto);
	if (fd < 0) {
		perror("L4 socket");
		return -1;
	}
	ref.r.s = fd;

	if (af == AF_INET) {
		if (bind_addr == BIND_LOOPBACK)
			addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		else if (bind_addr == BIND_EXT)
			addr4.sin_addr.s_addr = c->addr4;
		else
			addr4.sin_addr.s_addr = htonl(INADDR_ANY);

		sa = (const struct sockaddr *)&addr4;
		sl = sizeof(addr4);
	} else {
		if (bind_addr == BIND_LOOPBACK) {
			addr6.sin6_addr = in6addr_loopback;
		} else if (bind_addr == BIND_EXT) {
			addr6.sin6_addr = c->addr6;
		} else if (bind_addr == BIND_LL) {
			addr6.sin6_addr = c->addr6_ll;
			addr6.sin6_scope_id = c->ifi;
		} else {
			addr6.sin6_addr = in6addr_any;
		}

		sa = (const struct sockaddr *)&addr6;
		sl = sizeof(addr6);

		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
	}

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	if (bind(fd, sa, sl) < 0) {
		/* We'll fail to bind to low ports if we don't have enough
		 * capabilities, and we'll fail to bind on already bound ports,
		 * this is fine. This might also fail for ICMP because of a
		 * broken SELinux policy, see icmp_tap_handler().
		 */
		if (proto != IPPROTO_ICMP && proto != IPPROTO_ICMPV6) {
			close(fd);
			return 0;
		}
	}

	if (proto == IPPROTO_TCP && listen(fd, 128) < 0) {
		perror("TCP socket listen");
		close(fd);
		return -1;
	}

	ev.events = EPOLLIN;
	ev.data.u64 = ref.u64;
	if (epoll_ctl(c->epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		perror("L4 epoll_ctl");
		return -1;
	}

	return fd;
}

/**
 * sock_probe_mem() - Check if setting high SO_SNDBUF and SO_RCVBUF is allowed
 * @c:		Execution context
 */
void sock_probe_mem(struct ctx *c)
{
	int v = INT_MAX / 2, s;
	socklen_t sl;

	if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		c->low_wmem = c->low_rmem = 1;
		return;
	}

	sl = sizeof(v);
	if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v))	||
	    getsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, &sl) ||
	    (size_t)v < SNDBUF_BIG)
		c->low_wmem = 1;

	v = INT_MAX / 2;
	if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v))	||
	    getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &sl) ||
	    (size_t)v < RCVBUF_BIG)
		c->low_rmem = 1;

	close(s);
}


/**
 * timespec_diff_ms() - Report difference in milliseconds between two timestamps
 * @a:		Minuend timestamp
 * @b:		Subtrahend timestamp
 *
 * Return: difference in milliseconds
 */
int timespec_diff_ms(struct timespec *a, struct timespec *b)
{
	if (a->tv_nsec < b->tv_nsec) {
		return (b->tv_nsec - a->tv_nsec) / 1000000 +
		       (a->tv_sec - b->tv_sec - 1) * 1000;
	}

	return (a->tv_nsec - b->tv_nsec) / 1000000 +
	       (a->tv_sec - b->tv_sec) * 1000;
}

/**
 * bitmap_set() - Set single bit in bitmap
 * @map:	Pointer to bitmap
 * @bit:	Bit number to set
 */
void bitmap_set(uint8_t *map, int bit)
{
	unsigned long *word = (unsigned long *)map + BITMAP_WORD(bit);

	*word |= BITMAP_BIT(bit);
}

/**
 * bitmap_clear() - Clear single bit in bitmap
 * @map:	Pointer to bitmap
 * @bit:	Bit number to clear
 */
void bitmap_clear(uint8_t *map, int bit)
{
	unsigned long *word = (unsigned long *)map + BITMAP_WORD(bit);

	*word &= ~BITMAP_BIT(bit);
}

/**
 * bitmap_isset() - Check for set bit in bitmap
 * @map:	Pointer to bitmap
 * @bit:	Bit number to check
 *
 * Return: one if given bit is set, zero if it's not
 */
int bitmap_isset(const uint8_t *map, int bit)
{
	unsigned long *word = (unsigned long *)map + BITMAP_WORD(bit);

	return !!(*word & BITMAP_BIT(bit));
}

/**
 * line_read() - Similar to fgets(), no heap usage, a file instead of a stream
 * @buf:	Read buffer: on non-empty string, use that instead of reading
 * @len:	Maximum line length
 * @fd:		File descriptor for reading
 *
 * Return: @buf if a line is found, NULL on EOF or error
 */
char *line_read(char *buf, size_t len, int fd)
{
	int n, do_read = !*buf;
	char *p;

	if (!do_read) {
		char *nl;

		buf[len - 1] = 0;
		if (!strlen(buf))
			return NULL;

		p = buf + strlen(buf) + 1;

		while (*p == '\n' && strlen(p) && (size_t)(p - buf) < len)
			p++;

		if (!(nl = strchr(p, '\n')))
			return NULL;
		*nl = 0;

		return memmove(buf, p, len - (p - buf));
	}

	n = read(fd, buf, --len);
	if (n <= 0)
		return NULL;

	buf[len] = 0;

	p = buf;
	while (*p == '\n' && strlen(p) && (size_t)(p - buf) < len)
		p++;

	if (!(p = strchr(p, '\n')))
		return buf;

	*p = 0;
	if (p == buf)
		return buf;

	lseek(fd, (p - buf) - n + 1, SEEK_CUR);

	return buf;
}

/**
 * procfs_scan_listen() - Set bits for listening TCP or UDP sockets from procfs
 * @name:	Corresponding name of file under /proc/net/
 * @map:	Bitmap where numbers of ports in listening state will be set
 * @exclude:	Bitmap of ports to exclude from setting (and clear)
 */
void procfs_scan_listen(char *name, uint8_t *map, uint8_t *exclude)
{
	char line[BUFSIZ], path[PATH_MAX];
	unsigned long port;
	unsigned int state;
	int fd;

	snprintf(path, PATH_MAX, "/proc/net/%s", name);
	if ((fd = open(path, O_RDONLY)) < 0)
		return;

	*line = 0;
	line_read(line, sizeof(line), fd);
	while (line_read(line, sizeof(line), fd)) {
		/* NOLINTNEXTLINE(cert-err34-c): != 2 if conversion fails */
		if (sscanf(line, "%*u: %*x:%lx %*x:%*x %x", &port, &state) != 2)
			continue;

		/* See enum in kernel's include/net/tcp_states.h */
		if ((strstr(name, "tcp") && state != 0x0a) ||
		    (strstr(name, "udp") && state != 0x07))
			continue;

		if (bitmap_isset(exclude, port))
			bitmap_clear(map, port);
		else
			bitmap_set(map, port);
	}

	close(fd);
}

/**
 * ns_enter() - Enter configured network and user namespaces
 * @c:		Execution context
 *
 * Return: 0 on success, -1 on failure
 *
 * #syscalls:pasta setns
 */
int ns_enter(struct ctx *c)
{
	if (!c->netns_only && setns(c->pasta_userns_fd, CLONE_NEWUSER))
		return -errno;

	if (setns(c->pasta_netns_fd, CLONE_NEWNET))
		return -errno;

	return 0;
}
