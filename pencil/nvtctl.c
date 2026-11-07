/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nvt-netlink.h"
#include "nt36536.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	struct nvt_link link;
	uint8_t data[300];
	if (argc < 2 || (strcmp(argv[1], "send") && strcmp(argv[1], "monitor"))) {
		fprintf(stderr, "usage: %s send HEXBYTE... | monitor [seconds]\n", argv[0]);
		return 2;
	}
	int monitor = !strcmp(argv[1], "monitor");
	if (nvt_open(&link, monitor) < 0) {
		perror("nt36536 netlink");
		return 1;
	}
	if (!monitor) {
		int len = argc - 2;
		if (len > (int)sizeof(data))
			return 2;
		for (int i = 0; i < len; i++) {
			char *end;
			unsigned long value = strtoul(argv[i + 2], &end, 16);
			if (!*argv[i + 2] || *end || value > 255)
				return 2;
			data[i] = value;
		}
		if (nvt_send(&link, data, len) < 0) {
			perror("SEND rejected");
			nvt_close(&link);
			return 1;
		}
		if (len >= NVT_PEN_LOCAL_PREFIX_LEN &&
		    !memcmp(data, NVT_PEN_LOCAL_PREFIX, NVT_PEN_LOCAL_PREFIX_LEN))
			puts("SEND ACK: driver-local pen state applied (no SPI write)");
		else
			puts("SEND ACK: SPI write completed (not a firmware ACK)");
	} else {
		int seconds = argc > 2 ? atoi(argv[2]) : 30;
		long long end = now_ms() + (long long)seconds * 1000;
		unsigned long frames = 0;
		setvbuf(stdout, NULL, _IOLBF, 0);
		printf("listening family=%u group=%u seconds=%d\n", link.family, link.group, seconds);
		while (now_ms() < end) {
			struct pollfd pfd = { .fd = link.fd, .events = POLLIN };
			int timeout = (int)(end - now_ms());
			if (poll(&pfd, 1, timeout > 0 ? timeout : 0) <= 0)
				break;
			int n = nvt_receive(&link, data, sizeof(data));
			if (n < 0) {
				perror("receive");
				continue;
			}
			printf("%lld frame=%lu len=%d id=%u data=", now_ms(), ++frames, n, data[0] >> 3);
			for (int i = 0; i < n; i++)
				printf("%02x", data[i]);
			putchar('\n');
		}
		printf("frames=%lu\n", frames);
	}
	nvt_close(&link);
	return 0;
}
