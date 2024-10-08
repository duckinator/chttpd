#include <arpa/inet.h>
#include <errno.h>
#include <linux/sched.h>    // CLONE_* constants.
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h> // Can be removed when Debian defaults to Clang 15+.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>      // mount(), MS_* constants.
#include <sys/sendfile.h>
#include <sys/stat.h>       // fstat(), struct stat.
#include <sys/syscall.h>    // SYS_* constants.
#include <unistd.h>         // syscall(), maybe other things.

static char err404[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 17\r\n\r\nFile not found.\r\n";
static char err414[] = "HTTP/1.1 414 URI Too Long\r\nContent-Type: text/plain\r\nContent-Length: 57\r\n\r\nThe URI provided is too long for the server to process.\r\n";
static char err405[] = "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\nContent-Type: text/plain\r\nContent-Length: 37\r\n\r\nOnly GET/HEAD requests are allowed.\r\n";
static char err500[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: 40\r\n\r\nAn internal server error has occurred.\r\n";

// Size of send and receive buffers, in bytes.
#define BUF_SIZE (1024 * 1024 * 1)

#define EXT_OFFSET 10
char *exts[] = {
    // ext[n] = extension
    // (ext + n + EXT_OFFSET) = mime type
    //2345 67890
    ".css\0     text/css",
    ".html\0    text/html",
    ".jpeg\0    image/jpeg",
    ".jpg\0     image/jpeg",
    ".json\0    application/json",
    ".png\0     image/png",
    ".txt\0     text/plain",
    ".webm\0    video/webm",
    ".woff\0    font/woff",
};

#define MAX_EVENTS 10
static int BACKLOG = 50;
static int PORT = 8080;

static void LOG(char *msg) {
    printf("%s:%d:%s(): %s\n", __FILE__, __LINE__, __func__, msg);
}

volatile sig_atomic_t done = 0;
void prepare_to_exit(int _signal) {
    if (done)
        exit(EXIT_FAILURE);

    puts("Cleaning up. Press Ctrl-C again to exit immediately.");
    done = 1;
}

void register_signal_handler(void) {
    struct sigaction sigact;
    sigact.sa_handler = prepare_to_exit;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, NULL);

    sigact.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sigact, NULL);
}

void watch_socket(int epoll_fd, int sock_fd) {
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = sock_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event))
        perror("epoll_ctl");
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
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int)))
        perror("setsockopt");

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

void setcork(int fd, int optval) {
    if (setsockopt(fd, SOL_TCP, TCP_CORK, &optval, sizeof(int)))
        perror("setcork/setsockopt");
}

void send_chunk(int fd, char *response) {
    size_t length = strlen(response);
    if (send(fd, response, length, MSG_NOSIGNAL) == -1) {
        perror("send");
        if (errno == EPIPE)
            close(fd);
    }
}

int main(int argc, char *argv[]) {
    int epoll_fd = epoll_create1(0);
    LOG("Got epoll_fd.");

    if (epoll_fd == -1)
        pabort("epoll_create1");

    int server_fd = server_socket();
    LOG("Got server socket.");

    register_signal_handler();
    LOG("Registered signal handlers.");

    struct epoll_event events[MAX_EVENTS] = {0};
    watch_socket(epoll_fd, server_fd);
    LOG("Watching server_fd.");

    reroot("site");
    LOG("Isolated ./site as process mount root.");

    static char recvbuf[BUF_SIZE] = {0};
    static char sendbuf[BUF_SIZE] = {0};
    while (!done) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (num_events < 0) {
            perror("epoll_wait");
            continue;
        }

        for (size_t i = 0; i < num_events; i++) {
            int fd = events[i].data.fd;

            if (!(events[i].events & EPOLLIN)) {
                perror("epoll_wait");
                close(fd);
                continue;
            }

            if (server_fd == fd) {
                while (true) {
                    int client_fd = accept(server_fd, NULL, NULL);
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

            ssize_t count = read(fd, recvbuf, sizeof recvbuf - 1);
            recvbuf[sizeof(recvbuf) - 1] = '\0';

            if (count == 0 || count == -1) { // 0 = EOF, -1 = error
                if (count == -1 && errno != 0 && errno != EAGAIN && errno != EBADF) {
                    // if count == -1, read() failed.
                    // we don't care about the following errno values:
                    // - 0      (there was no error)
                    // - EAGAIN ("resource temporarily unavailable"
                    // - EBADF  ("bad file descriptor")
                    perror("read");
                }
                close(fd);
                break;
            }

            char *method = NULL;
            char *method_end = NULL;
            char *path = NULL;
            size_t path_size = -1;

            for (char *tmp = recvbuf; *tmp; tmp++) {
                if (!method && *tmp == ' ') {
                    method = recvbuf;
                    *tmp = '\0';
                    method_end = tmp;
                } else if (!path && (*tmp == ' ' || *tmp == '\r')) {
                    path = method_end + 1;
                    *tmp = '\0';
                    path_size = tmp - path;
                    break;
                }
            }

            if (!path) {
                if (method) {
                    // The path didn't fit in recvbuf, meaning it was too long.
                    send_chunk(fd, err414);
                    close(fd);
                }
                continue;
            }

            bool is_get = strncmp("GET", method, 4) == 0;
            bool is_head = strncmp("HEAD", method, 5) == 0;
            if (!is_get && !is_head) {
                // Non-GET/HEAD requests get a 405 error.
                send_chunk(fd, err405);
                close(fd);
                continue;
            }

            if (!path)
                continue;

            // GET or HEAD request.

            bool ends_with_slash = path[path_size - 1] == '/';
            if (ends_with_slash) {
                // Return /:dir/index.html for /:dir/.
                strncpy(path + path_size, "index.html", 11);
                path_size += 10;
            }

            int file_fd = open(path, O_RDONLY); // Try to open path.
            if (file_fd == -1) { // If opening it failed.
                perror("open");
                if (errno == ENOENT) // No such path - return 404.
                    send_chunk(fd, err404);
                else // All other errors - return 500.
                    send_chunk(fd, err500);
                close(fd);
                continue;
            }

            struct stat st;
            if (fstat(file_fd, &st)) { // Needed for catching /:dir.
                perror("fstat");
                close(file_fd);
                send_chunk(fd, err500);
                close(fd);
                continue;
            }

            // Redirect /:dir to /:dir/.
            if (!ends_with_slash && S_ISDIR(st.st_mode)) {
                close(file_fd);
                snprintf(sendbuf, sizeof sendbuf,
                        "HTTP/1.1 307 Temporary Redirect\r\n" \
                        "Location: %s/\r\n" \
                        "Content-Length: 0\r\n" \
                        "\r\n",
                        path);
                send_chunk(fd, sendbuf);
                close(fd);
                continue;
            }

            char *content_type = "application/octet-stream";

            char *ext = strchr(path, '.');
            if (ext) {
                for (size_t i = 0; i < (sizeof(exts) / sizeof(char*)); i++) {
                    if (strcmp(exts[i], ext) == 0) {
                        content_type = exts[i] + EXT_OFFSET;
                        break;
                    }
                }
            }

            uint64_t content_length = st.st_size;
            snprintf(sendbuf, sizeof sendbuf,
                    "HTTP/1.1 200 OK\r\n" \
                    "Content-Type: %s\r\n" \
                    "Content-Length: %lu\r\n" \
                    "Connection: close\r\n" \
                    "Server: chttpd <https://github.com/duckinator/chttpd>\r\n" \
                    "\r\n",
                    content_type, content_length);

            setcork(fd, 1); // put a cork in it

            send_chunk(fd, sendbuf);

            // For HEAD requests, only return the headers.
            if (is_get) {
                for (off_t offset = 0; offset < content_length;)
                    if (sendfile(fd, file_fd, &offset, content_length) == -1 &&
                            errno != EAGAIN) {
                        perror("sendfile");
                        break;
                    }
            }

            setcork(fd, 0); // release it all

            close(file_fd);

            // Read the rest of the request to avoid "connection reset by peer."
            if (count < sizeof(recvbuf))
                for (count = 1; count > 0;)
                    count = read(fd, recvbuf, sizeof recvbuf - 1);

            close(fd);
        }
    }

    LOG("Closing server socket.");
    close(server_fd);

    LOG("Closing epoll socket.");
    close(epoll_fd);

    return EXIT_SUCCESS;
}
