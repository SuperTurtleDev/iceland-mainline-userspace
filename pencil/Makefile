CC ?= cc
PREFIX ?= /usr
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Werror

GIO_CFLAGS := $(shell pkg-config --cflags gio-2.0 gio-unix-2.0 libdrm)
GIO_LIBS := $(shell pkg-config --libs gio-2.0 gio-unix-2.0 libdrm)

all: nvtctl iceland-pencild

nvtctl: nvtctl.o nvt-netlink.o

iceland-pencild.o: CFLAGS += $(GIO_CFLAGS)

iceland-pencild: iceland-pencild.o pen-protocol.o nvt-netlink.o
	$(CC) $(CFLAGS) $^ $(GIO_LIBS) -o $@

tests/protocol: tests/protocol.c pen-protocol.c pen-protocol.h nt36536.h
	$(CC) $(CFLAGS) -I. tests/protocol.c pen-protocol.c -o $@

tests/capture: tests/capture.c nvt-netlink.c nvt-netlink.h nt36536.h
	$(CC) $(CFLAGS) -I. tests/capture.c nvt-netlink.c -o $@

tests/status: tests/status.c iceland-pencild.c pen-protocol.c nvt-netlink.c pen-protocol.h nvt-netlink.h nt36536.h
	$(CC) $(CFLAGS) $(GIO_CFLAGS) -I. tests/status.c pen-protocol.c nvt-netlink.c $(GIO_LIBS) -o $@

check: tests/protocol tests/status
	./tests/protocol
	./tests/status

install: all
	install -Dm755 iceland-pencild $(DESTDIR)$(PREFIX)/libexec/iceland-pencild
	install -Dm755 nvtctl $(DESTDIR)$(PREFIX)/bin/nvtctl
	install -Dm644 iceland-pencild.service $(DESTDIR)$(PREFIX)/lib/systemd/system/iceland-pencild.service
	install -Dm644 99-iceland-pencil.quirks $(DESTDIR)$(PREFIX)/share/libinput/99-iceland-pencil.quirks
	install -Dm644 61-iceland-pencil.rules $(DESTDIR)$(PREFIX)/lib/udev/rules.d/61-iceland-pencil.rules

clean:
	rm -f nvtctl iceland-pencild *.o tests/protocol tests/capture tests/status

.PHONY: all clean check install
