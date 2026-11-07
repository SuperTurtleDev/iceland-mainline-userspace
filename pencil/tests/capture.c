/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Hardware integration capture: real evdev events alongside raw Netlink. */
#include "nvt-netlink.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static long long now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	struct nvt_link link;
	struct pollfd fds[3] = {{0}};
	unsigned long frames = 0, touch = 0, pen = 0, reports[2] = {0};
	int duration = argc > 1 ? atoi(argv[1]) : 60;
	if (nvt_open(&link, 1) < 0) {
		perror("netlink");
		return 1;
	}
	fds[0] = (struct pollfd){ .fd = link.fd, .events = POLLIN };
	fds[1].fd = fds[2].fd = -1;
	for (int i = 0; i < 64; i++) {
		char path[64], name[128] = {0};
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);
		int index = !strcmp(name, "NVTCapacitiveTouchScreen") ? 1 :
			    !strcmp(name, "NVTCapacitivePen") ? 2 : 0;
		if (index) {
			fds[index] = (struct pollfd){ .fd = fd, .events = POLLIN };
			printf("opened %s %s\n", path, name);
		} else {
			close(fd);
		}
	}
	if (fds[1].fd < 0 || fds[2].fd < 0) {
		fprintf(stderr, "missing evdev device\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	long long end = now() + duration * 1000LL;
	printf("capture started duration=%d\n", duration);
	while (now() < end) {
		int remaining = (int)(end - now());
		int ret = poll(fds, 3, remaining > 0 ? remaining : 0);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			break;
		if (fds[0].revents & POLLIN) {
			unsigned char data[300];
			int n = nvt_receive(&link, data, sizeof(data));
			if (n > 0) {
				printf("raw t=%lld n=%d id=%u pen=%02x data=", now(), n, data[0] >> 3,
				       n > 65 ? data[65] : 0);
				for (int i = 0; i < n; i++)
					printf("%02x", data[i]);
				putchar('\n');
				frames++;
			}
		}
		for (int i = 1; i < 3; i++) {
			if (!(fds[i].revents & POLLIN))
				continue;
			struct input_event events[128];
			ssize_t n = read(fds[i].fd, events, sizeof(events));
			for (size_t j = 0; n > 0 && j < (size_t)n / sizeof(*events); j++) {
				if (i == 1)
					touch++;
				else
					pen++;
				if (events[j].type == EV_SYN && events[j].code == SYN_REPORT)
					reports[i - 1]++;
				printf("evdev t=%lld device=%s type=%u code=%u value=%d\n", now(),
				       i == 1 ? "touch" : "pen", events[j].type, events[j].code, events[j].value);
			}
		}
	}
	printf("summary raw=%lu touch_events=%lu touch_reports=%lu pen_events=%lu pen_reports=%lu\n",
	       frames, touch, reports[0], pen, reports[1]);
	nvt_close(&link);
	close(fds[1].fd);
	close(fds[2].fd);
	return 0;
}
