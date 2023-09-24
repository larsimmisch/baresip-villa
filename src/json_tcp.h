/**
 * @file json_tcp.h  TCP json framing
 *
 * Copyright (C) 2023 Lars Immisch
 */

enum {
	DICT_BSIZE = 32,
	MAX_LEVELS =  8,
};

struct json_tcp;

typedef bool (json_tcp_frame_h)(const struct odict *od, int *errp, void *arg);

#ifdef __cplusplus
extern "C" {
#endif

/* send the dict od as json, terminated by \r\n. od will be destroyed when sent */
int json_tcp_send(struct json_tcp *json_tcp, struct odict *od);

int json_tcp_insert(struct json_tcp **json_tcpp, struct tcp_conn *tc,
		int layer, json_tcp_frame_h *frameh, void *arg);

#ifdef __cplusplus
}
#endif
