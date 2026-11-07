/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pen-protocol.h"
#include "nt36536.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	/* Actual Iceland IC report captured over the new Netlink interface. */
	const uint8_t timing[] = {0xf0,6,0,20,0x0b,0x14,0x78,0x35,0x31,0x0f,0x2f,0x0f,
		0xaa,0x0f,0x2f,0x0f,0x05,0x1e,0x70,0x0f,0x2f,0x0f,0x0f,0x0c};
	uint8_t packet[80];
	struct pen_command c;
	assert(pen_uplink(timing, sizeof(timing), packet, sizeof(packet)) == 24);
	assert(!memcmp(packet, (uint8_t[]){0xaa,2,1,20}, 4));
	assert(!memcmp(packet + 4, timing + 4, 20));
	assert(pen_uplink(timing, 10, packet, sizeof(packet)) == -1);
	assert(pen_uplink(timing, sizeof(timing), packet, 8) == -1);
	assert(pen_uplink((uint8_t[]){0x09,0,0,0}, 4, packet, sizeof(packet)) == 0);
	assert(pen_uplink((uint8_t[]){0xf0,6,1,2,0x0b,0x14}, 6, packet, sizeof(packet)) == 6);
	assert(!memcmp(packet, (uint8_t[]){0xaa,2,2,2,0x0b,0x14}, 6));
	/* Actual AA010206000B28661834 size reply; OEM pads its ioctl buffer. */
	assert(pen_downlink((uint8_t[]){0xaa,1,2,6,0,0x0b,0x28,0x66,0x18,0x34}, 10, &c) == 1);
	assert(c.len == 9 && !memcmp(c.data, (uint8_t[]){0x7f,0x40,0x24,0x0b,0x28,0x66,0x18,0x34,0}, 9));
	assert(pen_downlink((uint8_t[]){0xaa,1,1,2,0,0}, 6, &c) == 1);
	assert(c.kind == PEN_PRESSURE && c.len == 7);
	assert(!memcmp(c.data, (uint8_t[]){'N','V','T',0,NVT_PEN_LOCAL_PRESSURE,0,0}, 7));
	/* Actual pressure capture: 0x2134 must survive, not become a Boolean. */
	assert(pen_downlink((uint8_t[]){0xaa,1,1,2,0x34,0x21}, 6, &c) == 1);
	assert(!memcmp(c.data, (uint8_t[]){'N','V','T',0,NVT_PEN_LOCAL_PRESSURE,0x34,0x21}, 7));
	assert(pen_downlink((uint8_t[]){0xaa,1,1,2,0,0x40}, 6, &c) == 1 && c.data[6] == 0x40);
	assert(pen_downlink((uint8_t[]){0xaa,2,1,1,0}, 5, &c) == 1 && c.data[2] == 2);
	assert(pen_downlink((uint8_t[]){0xaa,2,2,1,1}, 5, &c) == 1 && c.data[2] == 0x23);
	assert(pen_downlink((uint8_t[]){0xaa,1,4,0}, 4, &c) == 1 && c.data[1] == 0x10);
	assert(pen_downlink((uint8_t[]){0xaa,1,7,1,0}, 5, &c) == 0);
	assert(pen_downlink((uint8_t[]){0xaa,1,1,3,0,0}, 6, &c) == -1);
	/* OEM click GATT is not the AA control protocol. */
	assert(pen_button((uint8_t[]){2}, 1, &c) == 1);
	assert(c.len == 6 && !memcmp(c.data, (uint8_t[]){'N','V','T',0,NVT_PEN_LOCAL_BUTTONS,1}, 6));
	const uint8_t release[] = {3,4,0xff};
	for (size_t i = 0; i < sizeof(release); i++) {
		assert(pen_button(release + i, 1, &c) == 1);
		assert(c.len == 6 && c.data[5] == 0);
	}
	assert(pen_button((uint8_t[]){1}, 1, &c) == 0); /* gesture, not a second button */
	assert(pen_button((uint8_t[]){0}, 1, &c) == 0);
	assert(pen_button(NULL, 0, &c) == -1);
	puts("protocol fixtures passed");
	return 0;
}
