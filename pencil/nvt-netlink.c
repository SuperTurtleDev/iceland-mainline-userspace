/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nvt-netlink.h"
#include "nt36536.h"

#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int attr_ok(const struct nlattr *a, size_t len)
{
	return len >= sizeof(*a) && a->nla_len >= sizeof(*a) &&
	       a->nla_len <= len && (size_t)NLA_ALIGN(a->nla_len) <= len;
}

static const void *attr_data(const struct nlattr *a)
{
	return (const char *)a + NLA_HDRLEN;
}

static size_t attr_len(const struct nlattr *a)
{
	return a->nla_len - NLA_HDRLEN;
}

static struct nlattr *next_attr(const struct nlattr *a, size_t *len)
{
	size_t step = NLA_ALIGN(a->nla_len);
	*len -= step;
	return (struct nlattr *)((char *)a + step);
}

static void add_attr(struct nlmsghdr *h, int type, const void *p, size_t len)
{
	struct nlattr *a = (void *)((char *)h + NLMSG_ALIGN(h->nlmsg_len));
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	memcpy((char *)a + NLA_HDRLEN, p, len);
	h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + NLA_ALIGN(a->nla_len);
}

static int request(struct nvt_link *link, int family, int cmd, int type,
		   const void *p, size_t len, int ack)
{
	union { struct nlmsghdr align; char data[512]; } buf = {0};
	struct nlmsghdr *h = (void *)buf.data;
	struct genlmsghdr *g = NLMSG_DATA(h);
	struct sockaddr_nl dest = { .nl_family = AF_NETLINK };

	if (len > 400) {
		errno = EMSGSIZE;
		return -1;
	}
	h->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	h->nlmsg_type = family;
	h->nlmsg_flags = NLM_F_REQUEST | (ack ? NLM_F_ACK : 0);
	h->nlmsg_seq = ++link->sequence;
	g->cmd = cmd;
	g->version = NVT_GENL_VERSION;
	add_attr(h, type, p, len);
	return sendto(link->fd, h, h->nlmsg_len, 0,
		      (void *)&dest, sizeof(dest)) < 0 ? -1 : 0;
}

/* No multicast on the command socket: ACK waits cannot consume raw events. */
static int reply(struct nvt_link *link, void *buf, size_t size)
{
	struct pollfd pfd = { .fd = link->fd, .events = POLLIN };
	int n;
	do {
		n = poll(&pfd, 1, 3000);
	} while (n < 0 && errno == EINTR);
	if (!n)
		errno = ETIMEDOUT;
	if (n <= 0)
		return -1;
	return recv(link->fd, buf, size, MSG_TRUNC);
}

static void group_id(struct nvt_link *link, const struct nlattr *groups)
{
	size_t left = attr_len(groups);
	const struct nlattr *group = attr_data(groups);
	for (; attr_ok(group, left); group = next_attr(group, &left)) {
		size_t remain = attr_len(group);
		const struct nlattr *a = attr_data(group);
		int match = 0;
		uint32_t id = 0;
		for (; attr_ok(a, remain); a = next_attr(a, &remain)) {
			int type = a->nla_type & NLA_TYPE_MASK;
			if (type == CTRL_ATTR_MCAST_GRP_NAME &&
			    attr_len(a) == sizeof(NVT_GENL_GROUP) &&
			    !memcmp(attr_data(a), NVT_GENL_GROUP, sizeof(NVT_GENL_GROUP)))
				match = 1;
			if (type == CTRL_ATTR_MCAST_GRP_ID && attr_len(a) == sizeof(id))
				memcpy(&id, attr_data(a), sizeof(id));
		}
		if (match)
			link->group = id;
	}
}

int nvt_open(struct nvt_link *link, int subscribe)
{
	union { struct nlmsghdr align; char data[8192]; } buf;
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
	struct nlmsghdr *h;
	int n, left;
	memset(link, 0, sizeof(*link));
	link->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (link->fd < 0)
		return -1;
	if (bind(link->fd, (void *)&addr, sizeof(addr)) < 0 ||
	    request(link, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME,
		    NVT_GENL_NAME, sizeof(NVT_GENL_NAME), 0) < 0)
		goto fail;
	n = reply(link, buf.data, sizeof(buf.data));
	if (n < 0)
		goto fail;
	if ((size_t)n > sizeof(buf.data)) {
		errno = EMSGSIZE;
		goto fail;
	}
	for (h = (void *)buf.data, left = n; NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
		if (h->nlmsg_seq != link->sequence)
			continue;
		if (h->nlmsg_type == NLMSG_ERROR && h->nlmsg_len >= NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
			errno = -((struct nlmsgerr *)NLMSG_DATA(h))->error;
			goto fail;
		}
		if (h->nlmsg_type != GENL_ID_CTRL || h->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
			continue;
		size_t remain = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
		const struct nlattr *a = (void *)((char *)NLMSG_DATA(h) + GENL_HDRLEN);
		for (; attr_ok(a, remain); a = next_attr(a, &remain)) {
			int type = a->nla_type & NLA_TYPE_MASK;
			if (type == CTRL_ATTR_FAMILY_ID && attr_len(a) == sizeof(link->family))
				memcpy(&link->family, attr_data(a), sizeof(link->family));
			if (type == CTRL_ATTR_MCAST_GROUPS)
				group_id(link, a);
		}
	}
	if (!link->family || (subscribe && !link->group)) {
		errno = ENOENT;
		goto fail;
	}
	if (subscribe && setsockopt(link->fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
				   &link->group, sizeof(link->group)) < 0)
		goto fail;
	return 0;
fail:
	n = errno;
	nvt_close(link);
	errno = n;
	return -1;
}

int nvt_send(struct nvt_link *link, const uint8_t *data, size_t len)
{
	union { struct nlmsghdr align; char data[8192]; } buf;
	struct nlmsghdr *h;
	int n, left;
	if (request(link, link->family, NVT_CMD_SEND, NVT_ATTR_DATA, data, len, 1) < 0)
		return -1;
	n = reply(link, buf.data, sizeof(buf.data));
	if (n < 0)
		return -1;
	if ((size_t)n <= sizeof(buf.data)) {
		for (h = (void *)buf.data, left = n; NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
			if (h->nlmsg_seq == link->sequence && h->nlmsg_type == NLMSG_ERROR &&
			    h->nlmsg_len >= NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
				n = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
				if (!n)
					return 0;
				errno = -n;
				return -1;
			}
		}
	}
	errno = EPROTO;
	return -1;
}

int nvt_receive(struct nvt_link *link, uint8_t *frame, size_t size)
{
	union { struct nlmsghdr align; char data[8192]; } buf;
	struct nlmsghdr *h;
	int n = recv(link->fd, buf.data, sizeof(buf.data), MSG_DONTWAIT | MSG_TRUNC), left;
	if (n < 0)
		return -1;
	if ((size_t)n > sizeof(buf.data)) {
		errno = EMSGSIZE;
		return -1;
	}
	for (h = (void *)buf.data, left = n; NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
		if (h->nlmsg_type != link->family || h->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
			continue;
		struct genlmsghdr *g = NLMSG_DATA(h);
		if (g->cmd != NVT_CMD_EVENT)
			continue;
		size_t remain = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
		const struct nlattr *a = (void *)((char *)g + GENL_HDRLEN);
		for (; attr_ok(a, remain); a = next_attr(a, &remain)) {
			if ((a->nla_type & NLA_TYPE_MASK) == NVT_ATTR_DATA) {
				if (attr_len(a) > size) {
					errno = EMSGSIZE;
					return -1;
				}
				memcpy(frame, attr_data(a), attr_len(a));
				return attr_len(a);
			}
		}
	}
	errno = EPROTO;
	return -1;
}

void nvt_close(struct nvt_link *link)
{
	if (link->fd >= 0)
		close(link->fd);
	link->fd = -1;
}
