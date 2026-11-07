/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pen-protocol.h"
#include "nt36536.h"
#include <string.h>

int pen_button(const uint8_t *p, size_t len, struct pen_command *c)
{
	memset(c, 0, sizeof(*c));
	if (!len)
		return -1;
	/* OEM z7.a.S / btadsorb.ble.s0.e. 01 is a separate double-click gesture. */
	switch (p[0]) {
	case 2:
	case 3:
	case 4:
	case 0xff:
		memcpy(c->data, NVT_PEN_LOCAL_PREFIX, NVT_PEN_LOCAL_PREFIX_LEN);
		c->data[4] = NVT_PEN_LOCAL_BUTTONS;
		c->data[5] = p[0] == 2;
		c->len = 6;
		return 1;
	default:
		return 0;
	}
}

/* OEM nvt_get_pen_uplink_timing and touch_pen_bt_uplink_package. */
int pen_uplink(const uint8_t *f, size_t len, uint8_t *packet, size_t capacity)
{
	if (len < 4)
		return -1;
	if ((f[0] >> 3) != 30 || f[1] != 6)
		return 0;
	if (f[2] > 1)
		return 0;
	size_t n = f[3];
	if (!n || n > 76 || n + 4 > len || n + 4 > capacity || (f[2] == 1 && n != 2))
		return -1;
	packet[0] = 0xaa;
	packet[1] = 2;
	packet[2] = f[2] + 1;
	packet[3] = n;
	memcpy(packet + 4, f + 4, n);
	return n + 4;
}

/* OEM touch_pen_set_cmd_map + nvt_set_pen_downlink, without Android IPC. */
int pen_downlink(const uint8_t *p, size_t len, struct pen_command *c)
{
	memset(c, 0, sizeof(*c));
	if (len < 4 || p[0] != 0xaa || len != (size_t)p[3] + 4)
		return -1;
	const uint8_t *data = p + 4;
	size_t n = p[3];
	uint8_t opcode;
	if (p[1] == 2 && (p[2] == 1 || p[2] == 2)) {
		if (!n)
			return -1;
		opcode = p[2] + 1;
	} else if (p[1] == 1) {
		switch (p[2]) {
		case 0: /* Configuration ID. */
			opcode = 1;
			break;
		case 1: /* Real little-endian tip pressure belongs to the pen sensor. */
			if (n != 2)
				return -1;
			c->kind = PEN_PRESSURE;
			memcpy(c->data, NVT_PEN_LOCAL_PREFIX, NVT_PEN_LOCAL_PREFIX_LEN);
			c->data[4] = NVT_PEN_LOCAL_PRESSURE;
			c->data[5] = data[0];
			c->data[6] = data[1];
			c->len = 7;
			return 1;
		case 2: { /* Geometry: OEM reads six bytes after the first response byte. */
			uint8_t geometry[7] = {0};
			if (n < 6 || n > sizeof(geometry))
				return -1;
			/* Match OEM's zero-initialised ioctl buffer for six-byte replies. */
			memcpy(geometry, data, n);
			c->data[0] = 0x7f;
			c->data[1] = 0x40;
			c->data[2] = 0x24;
			memcpy(c->data + 3, geometry + 1, 6);
			c->len = 9;
			return 1;
		}
		case 3: /* SPEED_SWITCH is a no-op in the reference chip driver. */
			return 0;
		case 4: /* Request IC timing again. */
			c->data[0] = 0x7f;
			c->data[1] = 0x10;
			c->data[2] = 1; /* PEN_UPLINK_IN_RESUME */
			c->len = 3;
			return 1;
		case 6: /* Pen information. */
			opcode = 8;
			break;
		default:
			return 0;
		}
		if (!n)
			return -1;
	} else {
		return 0;
	}
	c->data[0] = 0x7f;
	c->data[1] = 0x40;
	c->data[2] = ((data[0] & 7) << 5) | opcode;
	c->len = 3;
	return 1;
}
