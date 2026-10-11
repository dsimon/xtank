/*
 * Tests/phase2/sandbox_sendmsg_shim.c -- test-only LD_PRELOAD transport shim.
 *
 * This is NOT part of any shipped client or server.  It exists solely for the
 * restricted Linux CI sandbox used by the Phase 2 evidence runs, whose seccomp
 * filter denies sendmsg()/sendto() with an explicit destination but permits
 * send() on a socket that has been connect()ed (the same limitation P2.7/P2.8
 * worked around with a test-only -Wl,--wrap=sendmsg fixture).
 *
 * When sendmsg() with a destination fails with EPERM, this shim connect()s the
 * datagram socket to that destination and retries with send().  Because ENet
 * (pinned v1.3.18) fans a server socket out to several peers, the fallback is
 * only correct for the single-peer loopback/connected smoke here; it must not
 * be used as a general multi-peer transport.  On a host with unrestricted UDP
 * the first sendmsg() succeeds and this shim is a no-op.
 *
 * Build: gcc -shared -fPIC -O2 -o sandbox_sendmsg_shim.so \
 *            Tests/phase2/sandbox_sendmsg_shim.c -ldl
 * Use:   LD_PRELOAD=$PWD/sandbox_sendmsg_shim.so <client-or-server>
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>

static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
	static char flat[70000];
	ssize_t r;
	size_t n = 0;
	int i;

	if (real_sendmsg == NULL) {
		real_sendmsg = (ssize_t (*)(int, const struct msghdr *, int))
			dlsym(RTLD_NEXT, "sendmsg");
	}
	if (real_sendmsg == NULL) {
		errno = ENOSYS;
		return -1;
	}

	r = real_sendmsg(fd, msg, flags);
	if (r >= 0 || errno != EPERM || msg->msg_name == NULL) {
		return r;
	}

	if (connect(fd, (struct sockaddr *)msg->msg_name,
		    msg->msg_namelen) != 0 && errno != EISCONN) {
		return r;
	}
	for (i = 0; i < (int)msg->msg_iovlen; ++i) {
		if (n + msg->msg_iov[i].iov_len > sizeof(flat)) {
			errno = EMSGSIZE;
			return -1;
		}
		memcpy(flat + n, msg->msg_iov[i].iov_base,
		       msg->msg_iov[i].iov_len);
		n += msg->msg_iov[i].iov_len;
	}
	return send(fd, flat, n, flags);
}