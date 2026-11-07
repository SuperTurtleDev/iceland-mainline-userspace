/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NVT_NETLINK_CLIENT_H
#define NVT_NETLINK_CLIENT_H

#include <stddef.h>
#include <stdint.h>

struct nvt_link {
	int fd;
	uint16_t family;
	uint32_t group;
	uint32_t sequence;
};

int nvt_open(struct nvt_link *link, int subscribe);
int nvt_send(struct nvt_link *link, const uint8_t *data, size_t len);
int nvt_receive(struct nvt_link *link, uint8_t *frame, size_t size);
void nvt_close(struct nvt_link *link);

#endif
