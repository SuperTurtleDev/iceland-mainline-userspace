/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ICELAND_PEN_PROTOCOL_H
#define ICELAND_PEN_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

enum pen_command_kind { PEN_COMMAND, PEN_PRESSURE };
struct pen_command {
	uint8_t data[15];
	size_t len;
	enum pen_command_kind kind;
};

/* 1: translated, 0: unknown/not applicable, -1: malformed. */
int pen_uplink(const uint8_t *frame, size_t len, uint8_t *packet, size_t capacity);
int pen_downlink(const uint8_t *packet, size_t len, struct pen_command *command);
/* OEM click characteristic: physical down/up, not desktop gesture actions. */
int pen_button(const uint8_t *packet, size_t len, struct pen_command *command);

#endif
