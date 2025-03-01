# chttpd

A single-threaded HTTP/1.1 server written in C, which can manage over 55,000 requests per second on a mid-range desktop.

I got this performance by leaning heavily on Linux kernel features:
- `epoll` is used for I/O event notifications;
- `sendfile` is used to send files without an intermittent buffer;
- namespaces and `pivot_root` are used for filesystem isolation;
- `TCP_CORK` to group small `send` and `sendfile` calls together.

To elaborate on the namespace part a bit: the `./site/` directory becomes `/` for
the chttpd process, which means if you request `/index.html` the file is
_actually located at_ `/index.html` as far as the server is concerned.
No path processing needed.

I considered making it multithreaded, but honestly I'm not sure I even need to.

If you need more than 55,000 requests per second, use a real server like nginx. <3

## Port 80?

The code, as-is, doesn't need root. But what if you want to run it on port 80?

1. Change the port from `8080` to `80`.
2. (Re)build chttpd.
3. Run `setcap CAP_NET_BIND_SERVICE=+eip chttpd` (probably as root).

In theory it should work!

## Performance

System configuration:
- Ryzen 5 7600
- 32GB DDR5-5400 (chttpd uses less than 5MB on my system, though)
- 2TB nvme SSD.

Performance test for a large number of small requests:
- Consistently over 55,000 requests/second.
- Response times: 0.0045s slowest / 0.0009s average / 0.0001s fastest.
