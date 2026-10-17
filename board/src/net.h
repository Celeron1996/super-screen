/*
 * net.h — TCP 监听、收发与内核收包队列查询
 * See docs/plan-imx6ull.md §3.2
 */
#ifndef SUPER_SCREEN_NET_H
#define SUPER_SCREEN_NET_H

#include <stddef.h>
#include <stdint.h>

/* 监听 0.0.0.0:port（SO_REUSEADDR + TCP_NODELAY）；失败返回 -1。 */
int net_listen(uint16_t port);

/* 阻塞 accept；成功返回已连接 fd（已开 TCP_NODELAY），失败返回 -1。 */
int net_accept(int lfd);

/* 一次 recv；>0 字节数，0 对端关闭，-1 错误/超时。 */
int net_recv_some(int fd, void *buf, size_t cap);

/* ioctl(FIONREAD)：内核收包队列待读字节数；失败返回 -1。 */
int net_pending(int fd);

/* 循环发送直到 len 字节写完；成功 0，失败 -1。 */
int net_send_all(int fd, const void *buf, size_t len);

#endif /* SUPER_SCREEN_NET_H */
