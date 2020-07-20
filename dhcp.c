/* PASST - Plug A Simple Socket Transport
 *
 * dhcp.c - Minimalistic DHCP server for PASST
 *
 * Author: Stefano Brivio <sbrivio@redhat.com>
 * License: GPLv2
 *
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "passt.h"
#include "dhcp.h"
#include "util.h"

/**
 * struct opt - DHCP option
 * @force:	Force sending, even if the client didn't request it
 * @sent:	Convenience flag, set while filling replies
 * @slen:	Length of option defined for server
 * @s:		Option payload from server
 * @clen:	Length of option received from client
 * @c:		Option payload from client
 */
struct opt {
	int sent;
	int slen;
	unsigned char s[255];
	int clen;
	unsigned char c[255];
};

static struct opt opts[255] = {
	[1]  = { 0, 4, {  0 }, 0, { 0 }, },	/* Subnet mask */
	[3]  = { 0, 4, {  0 }, 0, { 0 }, },	/* Router */
	[6]  = { 0, 4, {  0 }, 0, { 0 }, },	/* DNS */
	[51] = { 0, 4, { 60 }, 0, { 0 }, },	/* Lease time */
	[53] = { 0, 1, {  0 }, 0, { 0 }, },	/* Message type */
#define DHCPDISCOVER	1
#define DHCPOFFER	2
#define DHCPREQUEST	3
#define DHCPDECLINE	4
#define DHCPACK		5
#define DHCPNAK		6
#define DHCPRELEASE	7
#define DHCPINFORM	8
#define DHCPFORCERENEW	9
	[54] = { 0, 4, {  0 }, 0, { 0 }, },	/* Server ID */
};

/**
 * struct msg - BOOTP/DHCP message
 * @op:		BOOTP message type
 * @htype:	Hardware address type
 * @hlen:	Hardware address length
 * @hops:	DHCP relay hops
 * @xid:	Transaction ID randomly chosen by client
 * @secs:	Seconds elapsed since beginning of acquisition or renewal
 * @flags:	DHCP message flags
 * @ciaddr:	Client IP address in BOUND, RENEW, REBINDING
 * @yiaddr:	IP address being offered or assigned
 * @siaddr:	Next server to use in bootstrap
 * @giaddr:	Relay agent IP address
 * @chaddr:	Client hardware address
 * @sname:	Server host name
 * @file:	Boot file name
 * @magic:	Magic cookie prefix before options
 * @o:		Options
 */
struct msg {
	uint8_t op;
#define BOOTREQUEST	1
#define BOOTREPLY	2
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
	uint32_t magic;
	uint8_t o[308];
} __attribute__((__packed__));

/**
 * fill_one() - Fill a single option in message
 * @m:		Message to fill
 * @o:		Option number
 * @offset:	Current offset within options field, updated on insertion
 */
static void fill_one(struct msg *m, int o, int *offset)
{
	m->o[*offset] = o;
	m->o[*offset + 1] = opts[o].slen;
	memcpy(&m->o[*offset + 2], opts[o].s, opts[o].slen);

	opts[o].sent = 1;
	*offset += 2 + opts[o].slen;
}

/**
 * fill() - Fill requested and forced options in message
 * @m:		Message to fill
 *
 * Return: current size of options field
 */
static int fill(struct msg *m)
{
	int i, o, offset = 0;

	m->op = BOOTREPLY;
	m->secs = 0;

	for (o = 0; o < 255; o++)
		opts[o].sent = 0;

	for (i = 0; i < opts[55].clen; i++) {
		o = opts[55].c[i];
		if (opts[o].slen)
			fill_one(m, o, &offset);
	}

	for (o = 0; o < 255; o++) {
		if (opts[o].slen && !opts[o].sent)
			fill_one(m, o, &offset);
	}

	m->o[offset++] = 255;
	m->o[offset++] = 0;

	if (offset < 62 /* RFC 951 */) {
		memset(&m->o[offset], 0, 62 - offset);
		offset = 62;
	}

	return offset;
}

/**
 * dhcp() - Check if this is a DHCP message, reply as needed
 * @c:		Execution context
 * @len:	Total L2 packet length
 * @eh:		Packet buffer, Ethernet header
 *
 * Return: 0 if it's not a DHCP message, 1 if handled, -1 on failure
 */
int dhcp(struct ctx *c, unsigned len, struct ethhdr *eh)
{
	struct iphdr *iph = (struct iphdr *)(eh + 1);
	struct udphdr *uh = (struct udphdr *)((char *)iph + iph->ihl * 4);
	struct msg *m = (struct msg *)(uh + 1);
	unsigned int i, mlen = len - sizeof(*eh) - sizeof(*iph);

	if (uh->dest != htons(67))
		return 0;

	if (mlen != ntohs(uh->len) || mlen < offsetof(struct msg, o) ||
	    m->op != BOOTREQUEST)
		return -1;

	for (i = 0; i < mlen - offsetof(struct msg, o); i += m->o[i + 1] + 2)
		memcpy(&opts[m->o[i]].c, &m->o[i + 2], m->o[i + 1]);

	if (opts[53].c[0] == DHCPDISCOVER) {
		fprintf(stderr, "DHCP: offer to discover");
		opts[53].s[0] = DHCPOFFER;
	} else if (opts[53].c[0] == DHCPREQUEST) {
		fprintf(stderr, "DHCP: ack to request");
		opts[53].s[0] = DHCPACK;
	} else {
		return -1;
	}

	fprintf(stderr, " from %02x:%02x:%02x:%02x:%02x:%02x\n\n",
		m->chaddr[0], m->chaddr[1], m->chaddr[2],
		m->chaddr[3], m->chaddr[4], m->chaddr[5]);

	m->yiaddr = c->addr4;
	*(unsigned long *)opts[1].s =  c->mask4;
	*(unsigned long *)opts[3].s =  c->gw4;
	*(unsigned long *)opts[6].s =  c->dns4;
	*(unsigned long *)opts[54].s = c->gw4;

	uh->len = htons(len = offsetof(struct msg, o) + fill(m));
	uh->check = 0;
	uh->source = htons(67);
	uh->dest = htons(68);

	iph->tot_len = htons(len += sizeof(*iph));
	iph->daddr = c->addr4;
	iph->saddr = c->gw4;
	iph->check = 0;
	iph->check = csum_ip4(iph, iph->ihl * 4);

	len += sizeof(*eh);
	memcpy(eh->h_dest, eh->h_source, ETH_ALEN);
	memcpy(eh->h_source, c->mac, ETH_ALEN);

	if (send(c->fd_unix, eh, len, 0) < 0)
		perror("DHCP: send");

	return 1;
}