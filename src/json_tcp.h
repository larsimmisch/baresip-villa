/**
 * @file json_tcp.h  TCP netstring framing
 *
 * Copyright (C) 2023 Lars Immisch
 */

struct json_tcp;

typedef bool (json_tcp_frame_h)(struct odict *od, int *errp, void *arg);

int json_tcp_send(struct json_tcp *json_tcp, const struct odict *od);

int json_tcp_insert(struct json_tcp **json_tcpp, struct tcp_conn *tc,
		int layer, json_tcp_frame_h *frameh, void *arg);
