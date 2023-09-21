/**
 * @file json_tcp.c  \r\n terminated JSON framing
 *
 * Copyright (C) 2023 Lars Immisch
 */

#include <math.h>
#include <string.h>

#include <re.h>

#define DEBUG_MODULE "json_tcp"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

#include "json_tcp.h"

enum {
	DICT_BSIZE = 32,
	MAX_LEVELS =  8,
};

struct json_tcp {
	struct tcp_conn *tc;
	struct tcp_helper *th;
	struct mbuf *rcvbuf;
	void *arg;

	json_tcp_frame_h *frameh;
	uint64_t n_tx;
	uint64_t n_rx;
};

static bool json_tcp_recv_handler(int *errp, struct mbuf *mbx, bool *estab,
			      void *arg)
{
	struct json_tcp *json_tcp = arg;
	(void)estab;

	if (!json_tcp->rcvbuf)
		json_tcp->rcvbuf = mbuf_alloc(mbx->size);

	struct mbuf *rcvbuf = json_tcp->rcvbuf;

	int err = mbuf_write_mem(rcvbuf, mbuf_buf(mbx), mbuf_get_left(mbx));
	if (err) {
		DEBUG_PRINTF("villa: failed to read into receive buffer (%m). Closing connection\n", err);
		mem_deref(json_tcp->tc);
		json_tcp->tc = NULL;
		*errp = ENOMEM;
		return true;
	}

	/* extract all json frames (delimited by NUL or \r\n\r\n) in the TCP-stream */
	for (size_t i = 0; i < mbuf_end(rcvbuf); ++i) {

		bool rn_delimited = (mbuf_end(rcvbuf) - i) >=2 &&
			strcmp((const char*)(rcvbuf->buf + i), "\r\n") == 0;

		if (rn_delimited) {

			++json_tcp->n_rx;

			struct odict *od;
			const char* str = (const char*)(rcvbuf->buf);
			err = json_decode_odict(&od, DICT_BSIZE, str, strlen(str),
				MAX_LEVELS);

			bool is_empty = odict_count(od, true) == 0;
			if (err || is_empty) {
				if (is_empty)
					DEBUG_PRINTF("villa: received JSON is empty. Closing connection\n", err);
				else
					DEBUG_PRINTF("villa: failed to decode JSON (%m). Closing connection\n", err);

				*errp = EINVAL;
				return true;
			}

			json_tcp->frameh(od, errp, json_tcp->arg);
			mbuf_rewind(json_tcp->rcvbuf);
		}
	}

	*errp = err;

	/* always handled */
	return true;
}

static void destructor(void *arg)
{
	struct json_tcp *json_tcp = arg;

	mem_deref(json_tcp->th);
	mem_deref(json_tcp->tc);
	if (json_tcp->rcvbuf)
		mem_deref(json_tcp->rcvbuf);
}

static int json_tcp_print_h(const char *p, size_t size, void *arg)
{
	struct mbuf *mb = arg;

	return mbuf_write_mem(mb, (const uint8_t*)p, size);
}

int json_tcp_send(struct json_tcp *json_tcp, const struct odict *od)
{
	struct mbuf *mb = mbuf_alloc(1024);

	struct re_printf pf = { json_tcp_print_h, mb };

	int err = json_encode_odict(&pf, od);
	if (err) {
		return err;
	}

	mbuf_write_str(mb, "\r\n");

	mbuf_set_pos(mb, 0);
	err = tcp_send(json_tcp->tc, mb);
	mem_deref(mb);

	return err;
}

static struct odict *json_tcp_hello(void)
{
	struct odict *od = NULL;
	odict_alloc(&od, DICT_BSIZE);
	if (!od)
		return NULL;

	odict_entry_add(od, "event", ODICT_BOOL, true);
	odict_entry_add(od, "type", ODICT_STRING, "version");
	odict_entry_add(od, "protocol_version", ODICT_INT, 1);
	odict_entry_add(od, "class", ODICT_STRING, "application");

	return od;
}

int json_tcp_insert(struct json_tcp **json_tcpp, struct tcp_conn *tc,
		int layer, json_tcp_frame_h *frameh, void *arg)
{
	struct json_tcp *json_tcp;
	int err;

	if (!json_tcpp || !tc || !frameh)
		return EINVAL;

	json_tcp = mem_zalloc(sizeof(*json_tcp), destructor);
	if (!json_tcp)
		return ENOMEM;

	json_tcp->tc = mem_ref(tc);
	err = tcp_register_helper(&json_tcp->th, tc, layer, NULL,
				  NULL, json_tcp_recv_handler, json_tcp);
	if (err)
		goto out;

	json_tcp->frameh = frameh;
	json_tcp->arg = arg;
	json_tcp->rcvbuf = NULL;

 out:
	if (err)
		mem_deref(json_tcp);
	else
		*json_tcpp = json_tcp;

	/* send hello with protocol version */
	struct odict* hello = json_tcp_hello();
	err = json_tcp_send(json_tcp, hello);
	mem_deref(hello);

	return err;
}
