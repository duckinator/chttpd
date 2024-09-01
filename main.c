#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
//#include <linux/limits.h>
#include <linux/sched.h>    // CLONE_* constants.
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <sched.h>
#include <sys/epoll.h>
//#include <sys/mman.h>
#include <sys/mount.h>      // mount(), MS_* constants.
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>       // fstat(), struct stat.
#include <sys/syscall.h>    // SYS_* constants.
#include <unistd.h>         // syscall(), chroot(), maybe other things.

#define LOG(level, msg) printf(level " %s:%d:%s(): %s\n", __FILE__, __LINE__, __func__, msg)
#define INFO(msg)   LOG("INFO ", msg)
#define WARN(msg)   LOG("WARN ", msg)

#ifdef DEBUG_MODE
#   define DEBUGF(fmt, ...) fprintf(stderr, level " %s:%d:%s(): " fmt, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#   define DEBUGF(fmt, ...) /* DEBUGF() */
#endif
#define DEBUG(msg) DEBUGF("%s", msg)

static char err404[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\nFile not found.\r\n";
static char err405[] = "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\nContent-Type: text/plain\r\n\r\nOnly GET/HEAD requests are allowed.\r\n";
static char err500[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nAn internal server error has occurred.\r\n";

#define EXT_OFFSET 10
char *exts[] = {
    // ext[n] = extension
    // (ext + n + EXT_OFFSET) = mime type
    //2345 67890
    ".html\0    text/html",
    ".css\0     text/css",
    ".json\0    application/json",
    ".txt\0     text/plain",
    ".woff\0    font/woff",
};

#define MAX_EVENTS 10
static int BACKLOG = 50;
static int PORT = 8080;

volatile sig_atomic_t done = 0;
void prepare_to_exit(int _signal) {
    if (done) {
        puts("Received Ctrl-C twice; exiting immediately!");
        exit(EXIT_FAILURE);
    }

    puts("Handling remaining connections, then exiting.");
    puts("Press Ctrl-C again to exit immediately.");
    done = 1;
}

void register_signal_handler(void) {
    struct sigaction sigact;
    sigact.sa_handler = prepare_to_exit;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, (struct sigaction *)NULL);

    sigact.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sigact, (struct sigaction *)NULL);
}

void watch_socket(int epoll_fd, int sock_fd) {
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = sock_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event)) {
        perror("epoll_ctl");
    }
}

void pabort(char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

void reroot(char *root) {
    if (syscall(SYS_unshare, CLONE_NEWUSER | CLONE_NEWNS))
        pabort("unshare");

    if (mount(root, root, NULL, MS_BIND | MS_PRIVATE, NULL))
        pabort("mount");

    if (syscall(SYS_pivot_root, root, root))
        pabort("pivot_root");

    if (chdir("/"))
        pabort("chdir");
}

int server_socket(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
        pabort("socket");

    // Mark address as reusable to avoid problems if client sockets aren't
    // all closed at exit.
    int optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int))) {
        perror("setsockopt");
    }


    if (fcntl(server_fd, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server, sizeof(server)))
        pabort("bind");

    if (listen(server_fd, BACKLOG))
        pabort("listen");

    return server_fd;
}

ssize_t send_chunk(int fd, char *response) {
    size_t length = strlen(response);
    ssize_t ret = send(fd, response, length, MSG_NOSIGNAL);
    if (ret == -1) {
        perror("send");
        if (errno == EPIPE)
            close(fd);
    }
    return ret;
}

int open_and_fstat(char *path, int *fd, struct stat *st) {
    *fd = open(path, O_RDONLY);
    if (*fd == -1) {
        perror("open");
        return 404;
    }

    if (fstat(*fd, st)) {
        perror("fstat");
        return 500;
    }

    return 0;
}


int main(int argc, char *argv[]) {
    int epoll_fd = epoll_create1(0);
    INFO("Got epoll_fd.");

    if (epoll_fd == -1)
        pabort("epoll_create1");

    int server_fd = server_socket();
    INFO("Got server socket.");

    register_signal_handler();
    INFO("Registered signal handlers.");

    struct epoll_event events[MAX_EVENTS] = {0};
    watch_socket(epoll_fd, server_fd);
    INFO("Watching server_fd.");

    reroot("site");
    INFO("Theoretically isolated ./site as process mount root.");

    while (!done) {
        DEBUG("epoll_wait()");
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (num_events < 0) {
            perror("epoll_wait");
            break;
        }
        DEBUGF("num_events = %i\n", num_events);

        for (size_t i = 0; i < num_events; i++) {
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))) {
                perror("epoll_wait");
                close(events[i].data.fd);
                continue;
            }

            if (server_fd == events[i].data.fd) {
                DEBUG("Handling server socket.");
                while (true) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    DEBUG("accept()");
                    if (client_fd == -1) {
                        // EAGAIN/EWOULDBLOCK aren't actual errors, so
                        // be quiet about it.
                        if ((errno != EAGAIN) && (errno != EWOULDBLOCK))
                            perror("accept");
                        break;
                    }

                    if (fcntl(client_fd, F_SETFL, O_NONBLOCK) == -1) {
                        perror("fcntl");
                        close(client_fd);
                        break;
                    }

                    watch_socket(epoll_fd, client_fd);
                }
                continue;
            }

            DEBUG("Handling client socket.");
            while (true) {
                char buf[513] = {0};
                DEBUGF("\nread(%i, buf, %lu) == ", events[i].data.fd, sizeof buf - 1);
                ssize_t count = read(events[i].data.fd, buf, sizeof buf - 1);
                DEBUGF("%li\n", count);
                buf[512] = '\0';

                if (count == 0 || count == -1) { // 0 = EOF, -1 = error
                    if (count == -1 && errno != 0 && errno != EAGAIN && errno != EBADF) {
                        // if count == -1, read() failed.
                        // we don't care about the following errno values:
                        // - 0      (there was no error)
                        // - EAGAIN ("resource temporarily unavailable"
                        // - EBADF  ("bad file descriptor")
                        perror("read");
                    }
                    close(events[i].data.fd);
                    break;
                }

                char *method = NULL;
                char *path = NULL;
                // e.g. "GET <path>"
                int scan_results = sscanf(buf, "%ms %ms", &method, &path);

                bool is_get = method && method[0] == 'G' && method[1] == 'E' && method[2] == 'T' && method[3] == '\0';
                bool is_head = method && method[0] == 'H' && method[1] == 'E' && method[2] == 'A' && method[3] == 'D' && method[4] == '\0';

                if (scan_results < 2) {
                    // Nothing to do.
                } else if (!is_get && !is_head) {
                    // Non-GET/HEAD requests get a 405 error.
                    send_chunk(events[i].data.fd, err405);
                    close(events[i].data.fd);
                } else if (path) {
                    // GET or HEAD request.
                    int fd = events[i].data.fd;

                    int file_fd = -1;
                    struct stat st = {0};

                    int error = open_and_fstat(path, &file_fd, &st);

                    size_t path_size = strlen(path);
                    if (!error && S_ISDIR(st.st_mode)) {
                        // Redirect /:dir to /:dir/ so relative URLs behave.
                        if (path[path_size - 1] != '/') {
                            char headers[256] = {0};
                            snprintf(headers, sizeof headers,
                                    "HTTP/1.1 307 Temporary Redirect\r\n" \
                                    "Location: %s/\r\n" \
                                    "Content-Length: 0\r\n" \
                                    "\r\n",
                                    path);
                            send_chunk(fd, headers);
                            close(fd);
                            continue;
                        }

                        // Return /:dir/index.html for /:dir/.
                        path = realloc(path, path_size + 12);
                        strncpy(path + path_size, "index.html", 11);
                        path_size += 10;

                        error = open_and_fstat(path, &file_fd, &st);
                    }

                    if (error == 404) {
                        send_chunk(fd, err404);
                        close(fd);
                        continue;
                    }

                    if (error) {
                        send_chunk(fd, err500);
                        close(fd);
                        continue;
                    }

                    char *ext = NULL;
                    for (size_t i = 0; i < path_size; i++)
                        if (path[i] == '.')
                            ext = path + i;

                    char *content_type = "application/octet-stream";
                    if (ext) {
                        for (size_t i = 0; i < sizeof exts; i++) {
                            char *tmp = exts[i];
                            bool match = true;
                            for (size_t j = 0; tmp[j] && match; j++)
                                match = tmp[j] == ext[j];

                            if (match) {
                                content_type = exts[i] + EXT_OFFSET;
                                break;
                            }
                        }
                    }

                    uint64_t content_length = (uint64_t)st.st_size;
                    char headers[256] = {0};
                    snprintf(headers, 256,
                            "HTTP/1.1 200 OK\r\n" \
                            "Content-Type: %s\r\n" \
                            "Content-Length: %lu\r\n" \
                            "Connection: close\r\n" \
                            "Server: chttpd <https://github.com/duckinator/chttpd>\r\n" \
                            "\r\n",
                            content_type, content_length);

                    send_chunk(fd, headers);

                    // For HEAD requests, only return the headers.
                    if (is_get)
                        sendfile(fd, file_fd, NULL, content_length);

                    close(file_fd);
                    close(fd);
                }

                free(method);
                free(path);
            } // while (true)
        }
    }

    INFO("Closing server socket.");
    close(server_fd);

    INFO("Closing epoll socket.");
    close(epoll_fd);

    return EXIT_SUCCESS;
}
