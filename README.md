# chttpd

A single-threaded HTTP/1.1 server written in C, which can manage over 20,000 requests per second on hardware from 2018.

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

If you need more than 20,000 requests per second, use a real server like nginx. <3

## Performance

System configuration:
- Ryzen 7 2700
- 32GB DDR4-3000 (chttpd uses less than 5MB on my system, though)
- 2TB nvme SSD.

Performance test for a large number of small requests:
- Consistently over 28,000 requests/second.
- Response times: 0.0082s slowest / 0.0018s average / 0.0004s fastest.
