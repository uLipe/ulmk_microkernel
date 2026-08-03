/* SPDX-License-Identifier: MIT */
#ifndef PROTOCOL_DUMMY_H
#define PROTOCOL_DUMMY_H

#include <stdint.h>

struct proto_msg {
	uint32_t label;
	uint32_t words[6];
};

int proto_disp_handle(struct proto_msg *msg);
int proto_input_handle(struct proto_msg *msg);
int proto_disp_on_state(void);
uintptr_t proto_disp_last_present(void);
uint32_t proto_disp_last_n_rects(void);
void *proto_disp_fb0(void);
void *proto_disp_fb1(void);

#endif /* PROTOCOL_DUMMY_H */
