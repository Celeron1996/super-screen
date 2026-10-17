/*
 * net.c — TCP 监听、收发与内核收包队列查询
 * See docs/plan-imx6ull.md §3.2
 */
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

int net_listen(uint16_t port)
{
    struct sockaddr_in addr;
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR("socket failed: %s", strerror(errno));
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        LOG_WARN("SO_REUSEADDR failed: %s", strerror(errno));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERR("bind :%u failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0) {
        LOG_ERR("listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    LOG_INFO("listening on 0.0.0.0:%u", port);
    return fd;
}

int net_accept(int lfd)
{
    int one = 1;
    int fd = accept(lfd, NULL, NULL);

    if (fd < 0) {
        return -1;
    }
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        LOG_WARN("TCP_NODELAY failed: %s", strerror(errno));
    }
    return fd;
}

int net_recv_some(int fd, void *buf, size_t cap)
{
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);

        if (n > 0) {
            return (int)n;
        }
        if (n == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

int net_pending(int fd)
{
    int bytes = 0;

    if (ioctl(fd, FIONREAD, &bytes) != 0) {
        return -1;
    }
    return bytes;
}

int net_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, flags);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}
