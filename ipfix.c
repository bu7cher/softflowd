/*
 * Copyright 2002 Damien Miller <djm@mindrot.org> All rights reserved.
 * Copyright 2012 Hitoshi Irino <irino@sfc.wide.ad.jp> All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "common.h"
#include "log.h"
#include "treetype.h"
#include "softflowd.h"

/* IPFIX a.k.a. Netflow v.10 */
struct IPFIX_HEADER {
	u_int16_t version, length;
	u_int32_t export_time;	/* in seconds */
	u_int32_t sequence, od_id;
} __packed;
struct IPFIX_SET_HEADER {
	u_int16_t set_id, length;
} __packed;
struct IPFIX_TEMPLATE_RECORD_HEADER {
	u_int16_t template_id, count;
} __packed;
struct IPFIX_TEMPLATE_SET_HEADER {
	struct IPFIX_SET_HEADER c;
	struct IPFIX_TEMPLATE_RECORD_HEADER r;
} __packed;
struct IPFIX_OPTION_TEMPLATE_SET_HEADER {
	struct IPFIX_SET_HEADER c;
	struct IPFIX_TEMPLATE_RECORD_HEADER r;
	u_int16_t scope_count;
} __packed;
struct IPFIX_FIELD_SPECIFIER {
	u_int16_t ie, length;
} __packed;

#define IPFIX_TEMPLATE_SET_ID		2
#define IPFIX_OPTION_TEMPLATE_SET_ID	3
#define IPFIX_MIN_RECORD_SET_ID	256

/* Flowset record ies the we care about */
#define IPFIX_octetDeltaCount		1
#define IPFIX_packetDeltaCount		2
/* ... */
#define IPFIX_protocolIdentifier	4
/* ... */
#define IPFIX_tcpControlBits		6
#define IPFIX_sourceTransportPort	7
#define IPFIX_sourceIPv4Address		8
/* ... */
#define IPFIX_ingressInterface		10
#define IPFIX_destinationTransportPort	11
#define IPFIX_destinationIPv4Address	12
/* ... */
#define IPFIX_egressInterface		14
/* ... */
#define IPFIX_flowEndSysUpTime		21
#define IPFIX_flowStartSysUpTime	22
/* ... */
#define IPFIX_sourceIPv6Address		27
#define IPFIX_destinationIPv6Address		28
/* ... */
#define IPFIX_ipVersion		60

#define IPFIX_meteringProcessId		143
#define IPFIX_systemInitTimeMilliseconds	160
#define PSAMP_selectorAlgorithm		304
#define PSAMP_samplingPacketInterval	305
#define PSAMP_samplingPacketSpace	306

#define PSAMP_selectorAlgorithm_count	1

/* Stuff pertaining to the templates that softflowd uses */
#define IPFIX_SOFTFLOWD_TEMPLATE_NRECORDS	13
struct IPFIX_SOFTFLOWD_TEMPLATE {
	struct IPFIX_TEMPLATE_SET_HEADER h;
	struct IPFIX_FIELD_SPECIFIER r[IPFIX_SOFTFLOWD_TEMPLATE_NRECORDS];
} __packed;

#define IPFIX_SOFTFLOWD_OPTION_TEMPLATE_SCOPE_RECORDS	1
#define IPFIX_SOFTFLOWD_OPTION_TEMPLATE_NRECORDS	4
struct IPFIX_SOFTFLOWD_OPTION_TEMPLATE {
	struct IPFIX_OPTION_TEMPLATE_SET_HEADER h;
	struct IPFIX_FIELD_SPECIFIER s[IPFIX_SOFTFLOWD_OPTION_TEMPLATE_SCOPE_RECORDS];
	struct IPFIX_FIELD_SPECIFIER r[IPFIX_SOFTFLOWD_OPTION_TEMPLATE_NRECORDS];
} __packed;

/* softflowd data set */
struct IPFIX_SOFTFLOWD_DATA_COMMON {
	u_int32_t flowEndSysUpTime, flowStartSysUpTime;
	u_int32_t octetDeltaCount, packetDeltaCount;
	u_int32_t ingressInterface, egressInterface;
	u_int16_t sourceTransportPort, destinationTransportPort;
	u_int8_t protocolIdentifier, tcpControlBits, ipVersion;
} __packed;

struct IPFIX_SOFTFLOWD_DATA_V4 {
	u_int32_t sourceIPv4Address, destinationIPv4Address;
	struct IPFIX_SOFTFLOWD_DATA_COMMON c;
} __packed;

struct IPFIX_SOFTFLOWD_DATA_V6 {
	//u_int8_t src_addr[16], dst_addr[16];
	struct in6_addr sourceIPv6Address, destinationIPv6Address;
	struct IPFIX_SOFTFLOWD_DATA_COMMON c;
} __packed;

struct IPFIX_SOFTFLOWD_OPTION_DATA {
	struct IPFIX_SET_HEADER c;	
	u_int32_t scope_pid;
	u_int64_t systemInitTimeMilliseconds;
	u_int16_t samplingAlgorithm;
	u_int16_t samplingInterval;
	u_int32_t samplingSpace;
} __packed;
	
/* Local data: templates and counters */
#define IPFIX_SOFTFLOWD_MAX_PACKET_SIZE	512
#define IPFIX_SOFTFLOWD_V4_TEMPLATE_ID	1024
#define IPFIX_SOFTFLOWD_V6_TEMPLATE_ID	2048
#define IPFIX_SOFTFLOWD_OPTION_TEMPLATE_ID	256

#define IPFIX_DEFAULT_TEMPLATE_INTERVAL	16

/* ... */
#define IPFIX_OPTION_SCOPE_SYSTEM    1
#define IPFIX_OPTION_SCOPE_INTERFACE 2
#define IPFIX_OPTION_SCOPE_LINECARD  3
#define IPFIX_OPTION_SCOPE_CACHE     4
#define IPFIX_OPTION_SCOPE_TEMPLATE  5
/* ... */
#define IPFIX_SAMPLING_ALGORITHM_DETERMINISTIC 1
#define IPFIX_SAMPLING_ALGORITHM_RANDOM        2
/* ... */

static struct IPFIX_SOFTFLOWD_TEMPLATE v4_template;
static struct IPFIX_SOFTFLOWD_TEMPLATE v6_template;
static struct IPFIX_SOFTFLOWD_OPTION_TEMPLATE option_template;
static struct IPFIX_SOFTFLOWD_OPTION_DATA option_data;
static int ipfix_pkts_until_template = -1;

static void
ipfix_init_template(void)
{
	bzero(&v4_template, sizeof(v4_template));
	v4_template.h.c.set_id = htons(IPFIX_TEMPLATE_SET_ID);
	v4_template.h.c.length = htons(sizeof(v4_template));
	v4_template.h.r.template_id = htons(IPFIX_SOFTFLOWD_V4_TEMPLATE_ID);
	v4_template.h.r.count = htons(IPFIX_SOFTFLOWD_TEMPLATE_NRECORDS);
	v4_template.r[0].ie = htons(IPFIX_sourceIPv4Address);
	v4_template.r[0].length = htons(4);
	v4_template.r[1].ie = htons(IPFIX_destinationIPv4Address);
	v4_template.r[1].length = htons(4);
	v4_template.r[2].ie = htons(IPFIX_flowEndSysUpTime);
	v4_template.r[2].length = htons(4);
	v4_template.r[3].ie = htons(IPFIX_flowStartSysUpTime);
	v4_template.r[3].length = htons(4);
	v4_template.r[4].ie = htons(IPFIX_octetDeltaCount);
	v4_template.r[4].length = htons(4);
	v4_template.r[5].ie = htons(IPFIX_packetDeltaCount);
	v4_template.r[5].length = htons(4);
	v4_template.r[6].ie = htons(IPFIX_ingressInterface);
	v4_template.r[6].length = htons(4);
	v4_template.r[7].ie = htons(IPFIX_egressInterface);
	v4_template.r[7].length = htons(4);
	v4_template.r[8].ie = htons(IPFIX_sourceTransportPort);
	v4_template.r[8].length = htons(2);
	v4_template.r[9].ie = htons(IPFIX_destinationTransportPort);
	v4_template.r[9].length = htons(2);
	v4_template.r[10].ie = htons(IPFIX_protocolIdentifier);
	v4_template.r[10].length = htons(1);
	v4_template.r[11].ie = htons(IPFIX_tcpControlBits);
	v4_template.r[11].length = htons(1);
	v4_template.r[12].ie = htons(IPFIX_ipVersion);
	v4_template.r[12].length = htons(1);

	bzero(&v6_template, sizeof(v6_template));
	v6_template.h.c.set_id = htons(IPFIX_TEMPLATE_SET_ID);
	v6_template.h.c.length = htons(sizeof(v6_template));
	v6_template.h.r.template_id = htons(IPFIX_SOFTFLOWD_V6_TEMPLATE_ID);
	v6_template.h.r.count = htons(IPFIX_SOFTFLOWD_TEMPLATE_NRECORDS);
	v6_template.r[0].ie = htons(IPFIX_sourceIPv6Address);
	v6_template.r[0].length = htons(16);
	v6_template.r[1].ie = htons(IPFIX_destinationIPv6Address);
	v6_template.r[1].length = htons(16);
	v6_template.r[2].ie = htons(IPFIX_flowEndSysUpTime);
	v6_template.r[2].length = htons(4);
	v6_template.r[3].ie = htons(IPFIX_flowStartSysUpTime);
	v6_template.r[3].length = htons(4);
	v6_template.r[4].ie = htons(IPFIX_octetDeltaCount);
	v6_template.r[4].length = htons(4);
	v6_template.r[5].ie = htons(IPFIX_packetDeltaCount);
	v6_template.r[5].length = htons(4);
	v6_template.r[6].ie = htons(IPFIX_ingressInterface);
	v6_template.r[6].length = htons(4);
	v6_template.r[7].ie = htons(IPFIX_egressInterface);
	v6_template.r[7].length = htons(4);
	v6_template.r[8].ie = htons(IPFIX_sourceTransportPort);
	v6_template.r[8].length = htons(2);
	v6_template.r[9].ie = htons(IPFIX_destinationTransportPort);
	v6_template.r[9].length = htons(2);
	v6_template.r[10].ie = htons(IPFIX_protocolIdentifier);
	v6_template.r[10].length = htons(1);
	v6_template.r[11].ie = htons(IPFIX_tcpControlBits);
	v6_template.r[11].length = htons(1);
	v6_template.r[12].ie = htons(IPFIX_ipVersion);
	v6_template.r[12].length = htons(1);
}

static void
ipfix_init_option(struct timeval *system_boot_time, struct OPTION *option) {
	bzero(&option_template, sizeof(option_template));
	option_template.h.c.set_id = htons(IPFIX_OPTION_TEMPLATE_SET_ID);
	option_template.h.c.length = htons(sizeof(option_template));
	option_template.h.r.template_id = htons(IPFIX_SOFTFLOWD_OPTION_TEMPLATE_ID);
	option_template.h.r.count = htons(IPFIX_SOFTFLOWD_OPTION_TEMPLATE_SCOPE_RECORDS + IPFIX_SOFTFLOWD_OPTION_TEMPLATE_NRECORDS);
	option_template.h.scope_count = htons(IPFIX_SOFTFLOWD_OPTION_TEMPLATE_SCOPE_RECORDS);
	option_template.s[0].ie = htons(IPFIX_meteringProcessId);
	option_template.s[0].length = htons(sizeof(option_data.scope_pid));
	option_template.r[0].ie = htons(IPFIX_systemInitTimeMilliseconds);
	option_template.r[0].length = htons(sizeof(option_data.systemInitTimeMilliseconds));
	option_template.r[1].ie = htons(PSAMP_selectorAlgorithm);
	option_template.r[1].length = htons(sizeof(option_data.samplingAlgorithm));
	option_template.r[2].ie = htons(PSAMP_samplingPacketInterval);
	option_template.r[2].length = htons(sizeof(option_data.samplingInterval));
	option_template.r[3].ie = htons(PSAMP_samplingPacketSpace);
	option_template.r[3].length = htons(sizeof(option_data.samplingSpace));

	bzero(&option_data, sizeof(option_data));
	option_data.c.set_id = htons(IPFIX_SOFTFLOWD_OPTION_TEMPLATE_ID);
	option_data.c.length = htons(sizeof(option_data));
	option_data.scope_pid = htonl((u_int32_t)option->meteringProcessId);
#if defined HAVE_HTOBE64
	option_data.systemInitTimeMilliseconds = htobe64((u_int64_t)system_boot_time->tv_sec * 1000 + (u_int64_t)system_boot_time->tv_usec / 1000);
#elif defined HAVE_HTONLL
	option_data.systemInitTimeMilliseconds = htonll((u_int64_t)system_boot_time->tv_sec * 1000 + (u_int64_t)system_boot_time->tv_usec / 1000);
#endif
	option_data.samplingAlgorithm = htons(PSAMP_selectorAlgorithm_count);
	option_data.samplingInterval = htons(1);
	option_data.samplingSpace = htonl(option->sample > 0 ? option->sample - 1 : 0);
}
static int
ipfix_flow_to_flowset(const struct FLOW *flow, u_char *packet, u_int len,
    u_int16_t ifidx, const struct timeval *system_boot_time, u_int *len_used)
{
	union {
		struct IPFIX_SOFTFLOWD_DATA_V4 d4;
		struct IPFIX_SOFTFLOWD_DATA_V6 d6;
	} d[2];
	struct IPFIX_SOFTFLOWD_DATA_COMMON *dc[2];
	u_int freclen, ret_len, nflows;

	bzero(d, sizeof(d));
	*len_used = nflows = ret_len = 0;
	switch (flow->af) {
	case AF_INET:
		freclen = sizeof(struct IPFIX_SOFTFLOWD_DATA_V4);
		memcpy(&d[0].d4.sourceIPv4Address, &flow->addr[0].v4, 4);
		memcpy(&d[0].d4.destinationIPv4Address, &flow->addr[1].v4, 4);
		memcpy(&d[1].d4.sourceIPv4Address, &flow->addr[1].v4, 4);
		memcpy(&d[1].d4.destinationIPv4Address, &flow->addr[0].v4, 4);
		dc[0] = &d[0].d4.c;
		dc[1] = &d[1].d4.c;
		dc[0]->ipVersion = dc[1]->ipVersion = 4;
		break;
	case AF_INET6:
		freclen = sizeof(struct IPFIX_SOFTFLOWD_DATA_V6);
		memcpy(&d[0].d6.sourceIPv6Address, &flow->addr[0].v6, 16);
		memcpy(&d[0].d6.destinationIPv6Address, &flow->addr[1].v6, 16);
		memcpy(&d[1].d6.sourceIPv6Address, &flow->addr[1].v6, 16);
		memcpy(&d[1].d6.destinationIPv6Address, &flow->addr[0].v6, 16);
		dc[0] = &d[0].d6.c;
		dc[1] = &d[1].d6.c;
		dc[0]->ipVersion = dc[1]->ipVersion = 6;
		break;
	default:
		return (-1);
	}

	dc[0]->flowStartSysUpTime = dc[1]->flowStartSysUpTime = 
	    htonl(timeval_sub_ms(&flow->flow_start, system_boot_time));
	dc[0]->flowEndSysUpTime = dc[1]->flowEndSysUpTime = 
	    htonl(timeval_sub_ms(&flow->flow_last, system_boot_time));
	dc[0]->octetDeltaCount = htonl(flow->octets[0]);
	dc[1]->octetDeltaCount = htonl(flow->octets[1]);
	dc[0]->packetDeltaCount = htonl(flow->packets[0]);
	dc[1]->packetDeltaCount = htonl(flow->packets[1]);
	dc[0]->ingressInterface = dc[0]->egressInterface = htonl(ifidx);
	dc[1]->ingressInterface = dc[1]->egressInterface = htonl(ifidx);
	dc[0]->sourceTransportPort = dc[1]->destinationTransportPort = flow->port[0];
	dc[1]->sourceTransportPort = dc[0]->destinationTransportPort = flow->port[1];
	dc[0]->protocolIdentifier = dc[1]->protocolIdentifier = flow->protocol;
	dc[0]->tcpControlBits = flow->tcp_flags[0];
	dc[1]->tcpControlBits = flow->tcp_flags[1];

	if (flow->octets[0] > 0) {
		if (ret_len + freclen > len)
			return (-1);
		memcpy(packet + ret_len, &d[0], freclen);
		ret_len += freclen;
		nflows++;
	}
	if (flow->octets[1] > 0) {
		if (ret_len + freclen > len)
			return (-1);
		memcpy(packet + ret_len, &d[1], freclen);
		ret_len += freclen;
		nflows++;
	}

	*len_used = ret_len;
	return (nflows);
}

/*
 * Given an array of expired flows, send netflow v9 report packets
 * Returns number of packets sent or -1 on error
 */
int
send_ipfix(struct FLOW **flows, int num_flows, int nfsock,
		u_int16_t ifidx, struct FLOWTRACKPARAMETERS *param,
		int verbose_flag)
{
	struct IPFIX_HEADER *ipfix;
	struct IPFIX_SET_HEADER *dh;
	struct timeval now;
	u_int offset, last_af, i, j, num_packets, inc, last_valid;
	socklen_t errsz;
	int err, r;
	u_char packet[IPFIX_SOFTFLOWD_MAX_PACKET_SIZE];
	struct timeval *system_boot_time = &param->system_boot_time;
	u_int64_t *flows_exported = &param->flows_exported;
	u_int64_t *packets_sent = &param->packets_sent;
	struct OPTION *option = &param->option;

	gettimeofday(&now, NULL);

	if (ipfix_pkts_until_template == -1) {
		ipfix_init_template();
		ipfix_pkts_until_template = 0;
		if (option != NULL){
			ipfix_init_option(system_boot_time, option);
		}
	}		

	last_valid = num_packets = 0;
	for (j = 0; j < num_flows;) {
		bzero(packet, sizeof(packet));
		ipfix = (struct IPFIX_HEADER *)packet;

		ipfix->version = htons(10);
		ipfix->length = 0; /* Filled as we go, htons at end */
		ipfix->export_time = htonl(time(NULL));
		ipfix->od_id = 0;
		offset = sizeof(*ipfix);

		/* Refresh template headers if we need to */
		if (ipfix_pkts_until_template <= 0) {
			memcpy(packet + offset, &v4_template,
			    sizeof(v4_template));
			offset += sizeof(v4_template);
			memcpy(packet + offset, &v6_template,
			    sizeof(v6_template));
			offset += sizeof(v6_template);
			if (option != NULL){
				memcpy(packet + offset, &option_template,
				       sizeof(option_template));
				offset += sizeof(option_template);
				memcpy(packet + offset, &option_data,
				       sizeof(option_data));
				offset += sizeof(option_data);
			}

			ipfix_pkts_until_template = IPFIX_DEFAULT_TEMPLATE_INTERVAL;
		}

		dh = NULL;
		last_af = 0;
		for (i = 0; i + j < num_flows; i++) {
			if (dh == NULL || flows[i + j]->af != last_af) {
				if (dh != NULL) {
					if (offset % 4 != 0) {
						/* Pad to multiple of 4 */
						dh->length += 4 - (offset % 4);
						offset += 4 - (offset % 4);
					}
					/* Finalise last header */
					dh->length = htons(dh->length);
				}
				if (offset + sizeof(*dh) > sizeof(packet)) {
					/* Mark header is finished */
					dh = NULL;
					break;
				}
				dh = (struct IPFIX_SET_HEADER *)
				    (packet + offset);
				dh->set_id =
				    (flows[i + j]->af == AF_INET) ?
				    v4_template.h.r.template_id : 
				    v6_template.h.r.template_id;
				last_af = flows[i + j]->af;
				last_valid = offset;
				dh->length = sizeof(*dh); /* Filled as we go */
				offset += sizeof(*dh);
			}

			r = ipfix_flow_to_flowset(flows[i + j], packet + offset,
			    sizeof(packet) - offset, ifidx, system_boot_time, &inc);
			if (r <= 0) {
				/* yank off data header, if we had to go back */
				if (last_valid)
					offset = last_valid;
				break;
			}
			offset += inc;
			dh->length += inc;
			last_valid = 0; /* Don't clobber this header now */
			if (verbose_flag) {
				logit(LOG_DEBUG, "Flow %d/%d: "
				    "r %d offset %d ie %04x len %d(0x%04x)",
				    r, i, j, offset, 
				    dh->set_id, dh->length, 
				    dh->length);
			}
		}
		/* Don't finish header if it has already been done */
		if (dh != NULL) {
			if (offset % 4 != 0) {
				/* Pad to multiple of 4 */
				dh->length += 4 - (offset % 4);
				offset += 4 - (offset % 4);
			}
			/* Finalise last header */
			dh->length = htons(dh->length);
		}
		ipfix->length = htons(offset);
		ipfix->sequence = htonl(*packets_sent + num_packets + 1);

		if (verbose_flag)
			logit(LOG_DEBUG, "Sending flow packet len = %d", offset);
		errsz = sizeof(err);
		/* Clear ICMP errors */
		getsockopt(nfsock, SOL_SOCKET, SO_ERROR, &err, &errsz); 
		if (send(nfsock, packet, (size_t)offset, 0) == -1)
			return (-1);
		num_packets++;
		ipfix_pkts_until_template--;

		j += i;
	}

	*flows_exported += j;
	return (num_packets);
}

void
ipfix_resend_template(void)
{
	if (ipfix_pkts_until_template > 0)
		ipfix_pkts_until_template = 0;
}