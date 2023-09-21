/**
 * @file villa_module.c Villa module
 *
 * Copyright (C) 2023 Lars Immisch
 */
#include <re.h>
#include <baresip.h>

#define DEBUG_MODULE "villa"
#define DEBUG_LEVEL 5

#include <re_dbg.h>

#include "json_tcp.h"

extern void villa_event_handler(struct ua *ua, enum ua_event ev,
	struct call *call, const char *prm, void *arg);

extern void call_event_handler(struct call *call, enum call_event ev,
	const char *str, void *arg);

extern int villa_status(struct re_printf *pf, void *arg);

enum { CTRL_PORT = 1235 };
struct ctrl_st {
	struct tcp_sock *ts;
	struct tcp_conn *tc;
	struct json_tcp *jt;
};

static struct ctrl_st *ctrl = NULL;  /* allow only one instance */

static const struct cmd cmdv[] = {
	{"villa", 0,       0, "villa status", villa_status },
};

static int print_handler(const char *p, size_t size, void *arg)
{
	struct mbuf *mb = arg;

	return mbuf_write_mem(mb, (uint8_t *)p, size);
}

static int encode_response(int cmd_error, struct mbuf *resp, const char *token)
{
	struct re_printf pf = {print_handler, resp};
	struct odict *od = NULL;
	char *buf = NULL;
	char m[256];
	int err;

	/* Empty response. */
	if (resp->pos == 0)
	{
		buf = mem_alloc(1, NULL);
		buf[0] = '\0';
	}
	else
	{
		resp->pos = 0;
		err = mbuf_strdup(resp, &buf, resp->end);
		if (err)
			return err;
	}

	err = odict_alloc(&od, 8);
	if (err)
		return err;

	err |= odict_entry_add(od, "response", ODICT_BOOL, true);
	err |= odict_entry_add(od, "ok", ODICT_BOOL, (bool)!cmd_error);

	if (cmd_error && str_len(buf) == 0)
		err |= odict_entry_add(od, "data", ODICT_STRING,
			str_error(cmd_error, m, sizeof(m)));
	else
		err |= odict_entry_add(od, "data", ODICT_STRING, buf);

	if (token)
		err |= odict_entry_add(od, "token", ODICT_STRING, token);

	if (err)
		goto out;

	mbuf_reset(resp);
	mbuf_init(resp);
	resp->pos = 0;

	err = json_encode_odict(&pf, od);
	if (err)
		DEBUG_WARNING("villa: failed to encode response JSON (%m)\n",
			err);

 out:
	mem_deref(buf);
	mem_deref(od);

	return err;
}

static bool command_handler(struct odict *od, int *errp, void *arg)
{
	struct ctrl_st *st = arg;
	struct mbuf *resp = mbuf_alloc(2048);
	struct re_printf pf = {print_handler, resp};
	const char *cmd, *prm, *tok;
	char buf[1024];
	int err;

	cmd = odict_string(od, "command");
	prm = odict_string(od, "params");
	tok = odict_string(od, "token");
	if (!cmd) {
		DEBUG_PRINTF("villa: missing json entries\n");
		*errp = EINVAL;
		goto out;
	}

	DEBUG_PRINTF("villa: handle_command:  cmd='%s', params:'%s', token='%s'\n",
	      cmd, prm, tok);

	re_snprintf(buf, sizeof(buf), "%s%s%s", cmd, prm ? " " : "", prm);

	resp->pos = 0;

	/* Relay message to long commands */
	err = cmd_process_long(baresip_commands(),
			       buf,
			       str_len(buf),
			       &pf, NULL);
	if (err) {
		DEBUG_WARNING("villa: error processing command (%m)\n", err);
	}

	err = encode_response(err, resp, tok ? tok : NULL);
	if (err) {
		DEBUG_WARNING("villa: failed to encode response (%m)\n", err);
		goto out;
	}

	resp->pos = 0;
	err = tcp_send(st->tc, resp);
	if (err) {
		DEBUG_WARNING("villa: failed to send the response (%m)\n", err);
	}

 out:
	mem_deref(resp);
	mem_deref(od);

	return true;  /* always handled */
}


static void tcp_close_handler(int err, void *arg)
{
	struct ctrl_st *st = arg;

	(void)err;

	// TODO: send connection closed to module

	if (st->tc)
		st->tc = mem_deref(st->tc);
}


static void tcp_conn_handler(const struct sa *peer, void *arg)
{
	struct ctrl_st *st = arg;

	(void)peer;

	/* only one connection allowed */
	st->tc = mem_deref(st->tc);
	st->jt = mem_deref(st->jt);

	(void)tcp_accept(&st->tc, st->ts, NULL, NULL, tcp_close_handler, st);
	(void)json_tcp_insert(&st->jt, st->tc, 0, command_handler, st);
}


/*
 * Relay UA events
 */
static void ua_event_handler(struct ua *ua, enum ua_event ev,
			     struct call *call, const char *prm, void *arg)
{
	struct ctrl_st *st = arg;
	struct mbuf *buf = mbuf_alloc(1024);
	struct re_printf pf = {print_handler, buf};
	struct odict *od = NULL;
	int err;

	buf->pos = 0;

	err = odict_alloc(&od, 8);
	if (err)
		return;

	err = odict_entry_add(od, "event", ODICT_BOOL, true);
	err |= event_encode_dict(od, ua, ev, call, prm);
	if (err) {
		DEBUG_WARNING("villa: failed to encode event (%m)\n", err);
		goto out;
	}

	err = json_encode_odict(&pf, od);
	if (err) {
		DEBUG_WARNING("villa: failed to encode event JSON (%m)\n", err);
		goto out;
	}

	if (st->tc) {
		buf->pos = 0;
		err = tcp_send(st->tc, buf);
		if (err) {
			DEBUG_WARNING("villa: failed to send event (%m)\n", err);
		}
	}

 out:
	mem_deref(buf);
	mem_deref(od);

	villa_event_handler(ua, ev, call, prm, arg);
}


static void message_handler(struct ua *ua, const struct pl *peer,
			    const struct pl *ctype,
			    struct mbuf *body, void *arg)
{
	struct ctrl_st *st = arg;
	struct mbuf *buf = mbuf_alloc(1024);
	struct re_printf pf = {print_handler, buf};
	struct odict *od = NULL;
	int err;

	buf->pos = 0;

	err = odict_alloc(&od, 8);
	if (err)
		return;

	err  = odict_entry_add(od, "message", ODICT_BOOL, true);
	err |= message_encode_dict(od, ua_account(ua), peer, ctype, body);
	if (err) {
		DEBUG_WARNING("villa: failed to encode message (%m)\n", err);
		goto out;
	}

	err = json_encode_odict(&pf, od);
	if (err) {
		DEBUG_WARNING("villa: failed to encode event JSON (%m)\n", err);
		goto out;
	}

	if (!st->tc)
		goto out;

	err = tcp_send(st->tc, buf);
	if (err) {
		DEBUG_WARNING("villa: failed to send the SIP message (%m)\n",
			err);
	}

out:
	mem_deref(buf);
	mem_deref(od);
}

static void ctrl_destructor(void *arg)
{
	struct ctrl_st *st = arg;

	mem_deref(st->tc);
	mem_deref(st->ts);
	mem_deref(st->jt);
}

static int ctrl_alloc(struct ctrl_st **stp, const struct sa *laddr)
{
	struct ctrl_st *st;
	int err;

	if (!stp)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), ctrl_destructor);
	if (!st)
		return ENOMEM;

	err = tcp_listen(&st->ts, laddr, tcp_conn_handler, st);
	if (err) {
		DEBUG_WARNING("villa: failed to listen on TCP %J (%m)\n",
			laddr, err);
		goto out;
	}

	DEBUG_PRINTF("ctrl_tcp: TCP socket listening on %J\n", laddr);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}

static int module_init(void)
{
	struct sa laddr;

	if (conf_get_sa(conf_cur(), "villa_tcp_listen", &laddr)) {
		sa_set_str(&laddr, "0.0.0.0", CTRL_PORT);
	}

	int err = ctrl_alloc(&ctrl, &laddr);
	if (err)
		return err;

	err = uag_event_register(ua_event_handler, ctrl);
	if (err)
		return err;

	// err = message_listen(baresip_message(), message_handler, ctrl);
	// if (err)
	// 	return err;

	err = cmd_register(baresip_commands(), cmdv, ARRAY_SIZE(cmdv));
	if (err)
		return err;

	DEBUG_PRINTF("villa: module loaded\n");

	return 0;
}


static int module_close(void)
{
	DEBUG_PRINTF("villa: module closing..\n");

	uag_event_unregister(ua_event_handler);
	cmd_unregister(baresip_commands(), cmdv);

	// message_unlisten(baresip_message(), message_handler);
	ctrl = mem_deref(ctrl);

	return 0;
}


const struct mod_export DECL_EXPORTS(villa) = {
	"villa",
	"application",
	module_init,
	module_close
};
