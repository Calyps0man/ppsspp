// PS3 IPv4 backend for the pinned aemu_postoffice dependency.
#include <errno.h>
#include <net/net.h>
#include <netinet/in.h>
#include <net/poll.h>
#include <string.h>
#include <net/socket.h>
#include <unistd.h>
#include "sock_impl.h"

void to_native_sock_addr(native_sock_addr *dst, const struct aemu_post_office_sock_addr *src) {
	memset(dst, 0, sizeof(*dst));
	dst->sin_len = sizeof(*dst);
	dst->sin_family = AF_INET;
	dst->sin_addr.s_addr = src->addr;
	dst->sin_port = src->port;
}

int native_connect_tcp_sock(void *addr, int addrlen) {
	if (!addr || addrlen != sizeof(native_sock_addr) || ((native_sock_addr *)addr)->sin_family != AF_INET)
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	int enabled = 1;
	int result;
	if (setsockopt(fd, SOL_SOCKET, SO_NBIO, &enabled, sizeof(enabled)) < 0) goto failed;
	result = connect(fd, (struct sockaddr *)addr, addrlen);
	if (result < 0) {
		if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK && errno != EAGAIN) goto failed;
		struct pollfd descriptor = {fd, POLLOUT, 0};
		// Poll waits at most five seconds. A nonzero SO_ERROR, regardless of the
		// SDK's BSD/newlib errno numbering, means connection failure.
		result = poll(&descriptor, 1, 5000);
		if (result <= 0 || !(descriptor.revents & POLLOUT) || (descriptor.revents & (POLLHUP | POLLNVAL))) goto failed;
		int error = 0;
		socklen_t length = sizeof(error);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) goto failed;
	}
	return fd;
failed:
	close(fd);
	return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
}

int native_close_tcp_sock(int fd) { return close(fd); }

static int receive(int fd, char *buf, int len, int flags) {
	int result;
	do { result = recv(fd, buf, len, flags); } while (result < 0 && errno == EINTR);
	if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
	return result;
}
int native_recv(int fd, char *buf, int len) { return receive(fd, buf, len, 0); }
int native_peek(int fd, char *buf, int len) { return receive(fd, buf, len, MSG_PEEK); }

static int transfer(int fd, char *buf, int len, bool non_block, bool *abort, bool sending) {
	if (len < 0 || !abort || (!buf && len)) { errno = EINVAL; return -1; }
	int offset = 0;
	while (offset < len) {
		if (*abort) return NATIVE_SOCK_ABORTED;
		int result = sending ? send(fd, buf + offset, len - offset, 0) : recv(fd, buf + offset, len - offset, 0);
		if (result > 0) { offset += result; continue; }
		if (result == 0) return sending ? -1 : 0;
		if (errno == EINTR) continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
		if (non_block && offset == 0) return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		// Once a packet is partly transferred it must finish before another
		// packet starts. Bounded waits retain the dependency's abort checks.
		struct pollfd descriptor = {fd, (short)(sending ? POLLOUT : POLLIN), 0};
		result = poll(&descriptor, 1, 10);
		if (result < 0 && errno != EINTR) return -1;
		if (result > 0 && (descriptor.revents & POLLNVAL)) return -1;
	}
	return offset;
}
int native_send_till_done(int fd, const char *buf, int len, bool non_block, bool *abort) {
	return transfer(fd, (char *)buf, len, non_block, abort, true);
}
int native_recv_till_done(int fd, char *buf, int len, bool non_block, bool *abort) {
	return transfer(fd, buf, len, non_block, abort, false);
}
bool native_send_buf_not_full(int fd) {
	struct pollfd descriptor = {fd, POLLOUT, 0};
	return poll(&descriptor, 1, 0) > 0 && (descriptor.revents & POLLOUT)
		&& !(descriptor.revents & (POLLHUP | POLLERR | POLLNVAL));
}
bool native_hung_up(int fd) {
	char byte;
	int result = native_peek(fd, &byte, 1);
	return result != 1 && result != AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
}
