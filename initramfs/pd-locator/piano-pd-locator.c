/*
 * piano-pd-locator — minimal Qualcomm SERVREG_LOC service locator.
 *
 * Written from scratch for the linux-xiaomi-piano bring-up initramfs.
 * It answers QMI "get domain list" queries sent by the kernel PDR client
 * (drivers/soc/qcom/pdr_interface.c), which pmic-glink (battery), the
 * audio stack (APR/GPR via "avs/audio") and slimbus use to map a service
 * name to remoteproc protection domains.
 *
 * Protocol facts (wire format, message and TLV ids) are taken from the
 * kernel UAPI header <linux/qrtr.h> and the QRTR/QMI conventions used by
 * the kernel PDR client; the domain table below mirrors the SM8750
 * service registry shipped in linux-firmware as qcom/sm8750 (the .jsn
 * files), which is byte-identical to the stock ROM copies under
 * local/firmware/non-hlos/image/ (see the session evidence log).
 *
 * Scope: single-purpose bring-up tool for one known platform.  It
 * intentionally serves a static table and implements exactly two message
 * ids (GET_DOMAIN_LIST, PFR).  No config files, no runtime state.
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


/* Minimal logging via write(2): avoids pulling vfprintf (and with it the
 * soft-float __*tf3 compiler-rt builtins) into the static binary. */
static void wr(const char *s, size_t n)
{
	ssize_t r = write(2, s, n);
	(void)r;
}

static void log_str(const char *a, const char *b)
{
	wr("pd-locator: ", 12);
	wr(a, strlen(a));
	if (b) {
		wr(": ", 2);
		wr(b, strlen(b));
	}
	wr("\n", 1);
}

static void log_u32(const char *a, unsigned v)
{
	char buf[12];
	char *p = buf + sizeof(buf);
	*--p = '\0';
	if (!v)
		*--p = '0';
	while (v) {
		*--p = '0' + (v % 10);
		v /= 10;
	}
	wr("pd-locator: ", 12);
	wr(a, strlen(a));
	wr(p, strlen(p));
	wr("\n", 1);
}

/* Linux QRTR UAPI essentials (include/uapi/linux/qrtr.h), restated here so
 * a bare musl sysroot is enough to cross-compile. */
#ifndef AF_QIPCRTR
#define AF_QIPCRTR	42
#endif

struct sockaddr_qrtr {
	uint16_t sq_family;
	uint32_t sq_node;
	uint32_t sq_port;
};

struct qrtr_ctrl_pkt {
	uint32_t cmd;		/* little endian on the wire */
	union {
		struct {
			uint32_t service;
			uint32_t instance;
			uint32_t node;
			uint32_t port;
		} server;
	};
};

#define QRTR_TYPE_NEW_SERVER	4
#define QRTR_PORT_CTRL		0xfffffffeU

/*
 * SERVREG_LOC QMI service identity, as queried by the kernel PDR client:
 * qmi_add_lookup(SERVREG_LOC service 64, version 1, instance 1) — the
 * QRTR "instance" field packs (instance << 8) or version, i.e. 0x101.
 */
#define SERVREG_LOC_SERVICE  64
#define SERVREG_LOC_INSTANCE 0x101

#define QMI_MSG_GET_DOMAIN_LIST 33
#define QMI_MSG_PFR             36

#define QMI_TLV_RESULT       2   /* { u16 result, u16 error } */
#define QMI_TLV_REQ_NAME     1   /* string, bare bytes */
#define QMI_TLV_TOTAL_DOMAINS 16 /* opt: { u8 valid, u16 total } */
#define QMI_TLV_DB_REVISION  17  /* opt: { u8 valid, u16 rev } */
#define QMI_TLV_DOMAIN_LIST  18  /* opt: { u8 valid, u8 count, entry[] } */

/* Domain table — SM8750 service registry (linux-firmware qcom/sm8750):
 *   adspr.jsn  msm/adsp/root_pd    74  tms/servreg
 *   adspua.jsn msm/adsp/audio_pd   74  tms/servreg, avs/audio
 *   adsps.jsn  msm/adsp/sensor_pd  74  tms/servreg
 *   adspuo.jsn msm/adsp/ois_pd     74  tms/servreg
 *   cdspr.jsn  msm/cdsp/root_pd    76  tms/servreg
 */
struct pd_entry {
	const char *service;	/* queried service name */
	const char *domain;	/* protection-domain path */
	uint32_t instance;	/* QRTR instance of the domain */
};

static const struct pd_entry pd_table[] = {
	{ "tms/servreg", "msm/adsp/root_pd",   74 },
	{ "tms/servreg", "msm/adsp/audio_pd",  74 },
	{ "tms/servreg", "msm/adsp/sensor_pd", 74 },
	{ "tms/servreg", "msm/adsp/ois_pd",    74 },
	{ "tms/servreg", "msm/cdsp/root_pd",   76 },
	{ "avs/audio",   "msm/adsp/audio_pd",  74 },
	{ NULL, NULL, 0 }
};

/* QMI service message header (packed, little endian). */
struct qmi_hdr {
	uint8_t  type;		/* 0 = request, 2 = response */
	uint16_t txn;
	uint16_t msg;
	uint16_t len;
} __attribute__((packed));

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

/* Append a TLV header; returns pointer past it. */
static uint8_t *put_tlv_hdr(uint8_t *p, uint8_t type, uint16_t len)
{
	*p++ = type;
	put_u16(p, len);
	return p + 2;
}

static void qmi_send_response(int fd, const struct sockaddr_qrtr *to,
			      const struct qmi_hdr *req,
			      const uint8_t *body, size_t body_len)
{
	uint8_t buf[1024];
	struct qmi_hdr *hdr = (struct qmi_hdr *)buf;
	struct sockaddr_qrtr dst = *to;
	ssize_t n;

	if (sizeof(*hdr) + body_len > sizeof(buf)) {
		log_str("response too long", NULL);
		return;
	}

	hdr->type = 2;			/* QMI response */
	hdr->txn = req->txn;
	hdr->msg = req->msg;
	hdr->len = body_len;
	memcpy(buf + sizeof(*hdr), body, body_len);

	dst.sq_family = AF_QIPCRTR;
	n = sendto(fd, buf, sizeof(*hdr) + body_len, 0,
		   (struct sockaddr *)&dst, sizeof(dst));
	if (n < 0)
		log_str("sendto", strerror(errno));
}

/* Extract the queried service name from a GET_DOMAIN_LIST request. */
static int parse_get_domain_list(const uint8_t *tlvs, size_t len,
				 char *name, size_t name_sz)
{
	size_t off = 0;

	while (off + 3 <= len) {
		uint8_t type = tlvs[off];
		uint16_t tlen = tlvs[off + 1] | (tlvs[off + 2] << 8);
		const uint8_t *val = tlvs + off + 3;

		if (off + 3 + tlen > len)
			return -1;
		if (type == QMI_TLV_REQ_NAME) {
			size_t cpy = tlen < name_sz - 1 ? tlen : name_sz - 1;
			memcpy(name, val, cpy);
			name[cpy] = '\0';
			return 0;
		}
		off += 3 + tlen;
	}
	return -1;
}

static void handle_get_domain_list(int fd, const struct sockaddr_qrtr *from,
				   const struct qmi_hdr *req,
				   const uint8_t *tlvs, size_t len)
{
	uint8_t body[900];
	uint8_t *p = body;
	uint8_t *tlv18_len_p, *count_p, *tlv18_body;
	char name[256];
	unsigned total = 0, matched = 0;
	const struct pd_entry *e;

	if (parse_get_domain_list(tlvs, len, name, sizeof(name)) < 0) {
		log_str("malformed GET_DOMAIN_LIST", NULL);
		return;
	}
	for (e = pd_table; e->service; e++)
		if (!strcmp(e->service, name))
			total++;

	/* TLV 2: result { u16 result=0, u16 error=0 } */
	p = put_tlv_hdr(p, QMI_TLV_RESULT, 4);
	put_u16(p, 0);
	put_u16(p + 2, 0);
	p += 4;

	/* TLV 16: total_domains */
	p = put_tlv_hdr(p, QMI_TLV_TOTAL_DOMAINS, 3);
	*p++ = 1;
	put_u16(p, total);
	p += 2;

	/* TLV 17: db_revision */
	p = put_tlv_hdr(p, QMI_TLV_DB_REVISION, 3);
	*p++ = 1;
	put_u16(p, 1);
	p += 2;

	/* TLV 18: domain_list { u8 valid=1, u8 count, entries } */
	p = put_tlv_hdr(p, QMI_TLV_DOMAIN_LIST, 0);	/* length fixed below */
	tlv18_len_p = p - 2;
	*p++ = 1;
	count_p = p++;
	tlv18_body = p;

	for (e = pd_table; e->service; e++) {
		size_t dlen = strlen(e->domain);
		if (strcmp(e->service, name))
			continue;
		/* entry: { u8 strlen, char str[], u32 instance,
		 *          u8 service_data_valid=0, u32 service_data=0 }
		 */
		if (p + 1 + dlen + 4 + 1 + 4 > body + sizeof(body)) {
			log_str("domain list overflow", NULL);
			return;
		}
		*p++ = dlen;
		memcpy(p, e->domain, dlen);
		p += dlen;
		put_u32(p, e->instance);
		p += 4;
		*p++ = 0;
		put_u32(p, 0);
		p += 4;
		matched++;
	}

	*count_p = matched;
	put_u16(tlv18_len_p, (uint16_t)(1 + 1 + (p - tlv18_body)));

	log_u32(name, matched); /* service name -> matched domains */
	qmi_send_response(fd, from, req, body, p - body);
}

static void handle_pfr(int fd, const struct sockaddr_qrtr *from,
		       const struct qmi_hdr *req)
{
	uint8_t body[7];
	uint8_t *p = body;

	p = put_tlv_hdr(p, QMI_TLV_RESULT, 4);
	put_u16(p, 0);
	put_u16(p + 2, 0);

	qmi_send_response(fd, from, req, body, 7);
}

static int announce_service(int fd)
{
	struct sockaddr_qrtr me;
	struct qrtr_ctrl_pkt pkt;
	socklen_t sl = sizeof(me);
	ssize_t n;

	if (getsockname(fd, (struct sockaddr *)&me, &sl) < 0) {
		log_str("getsockname", strerror(errno));
		return -1;
	}

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_SERVER;
	pkt.server.service = SERVREG_LOC_SERVICE;
	pkt.server.instance = SERVREG_LOC_INSTANCE;
	pkt.server.node = me.sq_node;
	pkt.server.port = me.sq_port;

	me.sq_port = QRTR_PORT_CTRL;
	n = sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&me, sizeof(me));
	if (n < 0) {
		log_str("NEW_SERVER", strerror(errno));
		return -1;
	}
	log_str("SERVREG_LOC published", NULL);
	return 0;
}

int main(void)
{
	struct sockaddr_qrtr from;
	uint8_t buf[2048];
	struct pollfd pfd;
	int fd, retry;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) {
		log_str("socket(AF_QIPCRTR) failed, qrtr module loaded?", strerror(errno));
		return 1;
	}

	/* qrtr.ko may still be probing when we start; retry the announcement
	 * for a few seconds — the kernel PDR client retries its lookups
	 * periodically, so an early failure is recoverable either way.
	 */
	for (retry = 0; retry < 30; retry++) {
		if (announce_service(fd) == 0)
			break;
		sleep(1);
	}
	if (retry == 30)
		return 1;

	for (;;) {
		struct qmi_hdr *hdr;
		socklen_t sl = sizeof(from);
		ssize_t n;

		pfd.fd = fd;
		pfd.events = POLLIN;
		n = poll(&pfd, 1, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			log_str("poll", strerror(errno));
			return 1;
		}

		n = recvfrom(fd, buf, sizeof(buf), 0,
			     (struct sockaddr *)&from, &sl);
		if (n < 0) {
			if (errno == EINTR || errno == ENETRESET)
				continue;
			log_str("recvfrom", strerror(errno));
			continue;
		}

		/* Control-port traffic is not ours to answer. */
		if (from.sq_port == (uint32_t)QRTR_PORT_CTRL || n < (ssize_t)sizeof(*hdr))
			continue;

		hdr = (struct qmi_hdr *)buf;
		if (hdr->msg == QMI_MSG_GET_DOMAIN_LIST)
			handle_get_domain_list(fd, &from, hdr,
						buf + sizeof(*hdr),
						n - sizeof(*hdr));
		else if (hdr->msg == QMI_MSG_PFR)
			handle_pfr(fd, &from, hdr);
	}

	close(fd);
	return 0;
}
