#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <linux/sched.h>    // CLONE_* constants.
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>      // mount(), MS_* constants.
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>       // fstat(), struct stat.
#include <sys/syscall.h>    // SYS_* constants.
#include <unistd.h>         // syscall(), maybe other things.

static char err404[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 17\r\n\r\nFile not found.\r\n";
static char err414[] = "HTTP/1.1 414 URI Too Long\r\nContent-Type: text/plain\r\nContent-Length: 57\r\n\r\nThe URI provided is too long for the server to process.\r\n";
static char err405[] = "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\nContent-Type: text/plain\r\nContent-Length: 37\r\n\r\nOnly GET/HEAD requests are allowed.\r\n";
static char err500[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: 40\r\n\r\nAn internal server error has occurred.\r\n";

// Maximum length of a request path.
#define MAX_PATH_SIZE 512

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

enum event_type_e {
    EVENT_TYPE_ACCEPT,
    EVENT_TYPE_READ,
    EVENT_TYPE_WRITE,
};

typedef struct request {
    int event_type;
    int client_fd;
    int iovec_count;
    struct iovec iov[];
} request_t;

struct io_uring ring;


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

void request_accept(int server_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, server_fd, NULL, NULL, 0);
    request_t *req = malloc(sizeof(request_t));
    req->event_type = EVENT_TYPE_ACCEPT;
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

#define READ_SIZE 8192
void request_read(int client_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    request_t *req = malloc(sizeof(request_t) + sizeof(struct iovec));
    req->iov[0].iov_base = malloc(READ_SIZE);
    req->iov[0].iov_len = READ_SIZE;
    req->event_type = EVENT_TYPE_READ;
    req->client_fd = client_fd;
    memset(req->iov[0].iov_base, 0, READ_SIZE);
    /* Linux kernel 5.5 has support for readv, but not for recv() or read() */
    io_uring_prep_readv(sqe, client_fd, &req->iov[0], 1, 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

void request_write(request_t *req) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    req->event_type = EVENT_TYPE_WRITE;
    io_uring_prep_writev(sqe, req->client_fd, req->iov, req->iovec_count, 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

void send_chunk(int client_fd, char *response) {
    struct request *req = malloc(sizeof(*req) + sizeof(struct iovec));
    unsigned long len = strlen(response);
    req->iovec_count = 1;
    req->client_fd = client_fd;
    req->iov[0].iov_base = malloc(len);
    req->iov[0].iov_len = len;
    memcpy(req->iov[0].iov_base, response, len);
    request_write(req);
}


void uring_sendfile(int fd, off_t file_size, struct iovec *iov) {
    char *buf = malloc(file_size + 1);
    buf[file_size] = '\0';

    /* We should really check for short reads here */
    int ret = read(fd, buf, file_size);
    if (ret < file_size) {
        fprintf(stderr, "Encountered a short read (%d vs %lu).\n", ret, file_size);
    }

    iov->iov_base = buf;
    iov->iov_len = file_size;
}

void handle_client(request_t *req) {
    char *method = NULL;
    char *method_end = NULL;
    char *path = NULL;
    size_t path_size = -1;

    char *recvbuf = req->iov[0].iov_base;
    size_t len = req->iov[0].iov_len;

    recvbuf[len - 1] = '\0';
    for (char *tmp = recvbuf; *tmp; tmp++) {
        if (!method && *tmp == ' ') {
            method = recvbuf;
            *tmp = '\0';
            method_end = tmp;
        } else if (!path && (*tmp == ' ' || *tmp == '\r' || *tmp == '\n')) {
            path = method_end + 1;
            *tmp = '\0';
            path_size = tmp - path;
            break;
        }
    }

    //printf("method = '%s'\npath   = '%s'\n", method, path);

    if (method && !path) {
        //printf("  method && !path => err414\n");
        // The path didn't fit in recvbuf, meaning it was too long.
        send_chunk(req->client_fd, err414);
        //close(req->client_fd);
        return;
    }

    if (!method) {
        //printf("method is empty\n");
        return;
    }

    //printf("  method && path\n");

    bool is_get = strncmp("GET", method, 4) == 0;
    bool is_head = strncmp("HEAD", method, 5) == 0;
    if (!is_get && !is_head) {
        // Non-GET/HEAD requests get a 405 error.
        send_chunk(req->client_fd, err405);
        //close(req->client_fd);
        return;
    }

    if (!path) {
        printf("path is empty\n");
        return;
    }

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
            send_chunk(req->client_fd, err404);
        else // All other errors - return 500.
            send_chunk(req->client_fd, err500);
        //close(req->client_fd);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st)) { // Needed for catching /:dir.
        perror("fstat");
        close(file_fd);
        send_chunk(req->client_fd, err500);
        //close(req->client_fd);
        return;
    }

    // Redirect headers are 67 bytes longer than the path, plus need a null byte.
    char *sendbuf = malloc(MAX_PATH_SIZE + 68);

    // Redirect /:dir to /:dir/.
    if (!ends_with_slash && S_ISDIR(st.st_mode)) {
        close(file_fd);
        snprintf(sendbuf, sizeof sendbuf,
                "HTTP/1.1 307 Temporary Redirect\r\n" \
                "Location: %s/\r\n" \
                "Content-Length: 0\r\n" \
                "\r\n",
                path);
        send_chunk(req->client_fd, sendbuf);
        close(file_fd);
        //close(req->client_fd);
        return;
    }

    char *content_type = "application/octet-stream";

    char *ext = strchr(path, '.');
    if (ext) {
        for (size_t i = 0; i < sizeof(exts); i++) {
            if (strcmp(exts[i], ext) == 0) {
                content_type = exts[i] + EXT_OFFSET;
                break;
            }
        }
    }

    uint64_t content_length = st.st_size;
    snprintf(sendbuf, 256,
            "HTTP/1.1 200 OK\r\n" \
            "Content-Type: %s\r\n" \
            "Content-Length: %lu\r\n" \
            "Connection: close\r\n" \
            "Server: chttpd <https://github.com/duckinator/chttpd>\r\n" \
            "\r\n",
            content_type, content_length);

    //setcork(req->client_fd, 1); // put a cork in it

    send_chunk(req->client_fd, sendbuf);

    // For HEAD requests, only return the headers.
    if (is_get) {
        //sendfile(fd, file_fd, NULL, content_length);
        req->iovec_count += 1;
        req = realloc(req, sizeof(request_t) * (sizeof(struct iovec) * req->iovec_count));
        uring_sendfile(file_fd, content_length, &(req->iov[req->iovec_count - 1]));
    }

    request_write(req);

    close(file_fd);
    //close(req->client_fd);
}

int main(int argc, char *argv[]) {
    int server_fd = server_socket();
    LOG("Got server socket.");

    register_signal_handler();
    LOG("Registered signal handlers.");

    reroot("site");
    LOG("Theoretically isolated ./site as process mount root.");

    struct io_uring_cqe *cqe;
    #define QUEUE_DEPTH 256
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

    request_accept(server_fd);

    while (!done) {
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0)
            pabort("io_uring_wait_cqe");

        struct request *req = (struct request*)cqe->user_data;

        if (cqe->res < 0) {
/*            fprintf(stderr, "Async request failed: %s for event: %d\n",
                    strerror(-cqe->res), req->event_type);*/
            //continue;
        }

        switch (req->event_type) {
        case EVENT_TYPE_ACCEPT:
            request_accept(server_fd);
            request_read(cqe->res);
            break;
        case EVENT_TYPE_READ:
            if (cqe->res) {
                handle_client(req);
                request_write(req);
                //free(req->iov[0].iov_base);
            }
            break;
        case EVENT_TYPE_WRITE:
            /*for (int i = 0; i < req->iovec_count; i++) {
                if (req->iov[i].iov_base)
                    free(req->iov[i].iov_base);
            }*/
            close(req->client_fd);
            break;
        default:
            fprintf(stderr, "Unknown event type: %d\n", req->event_type);
            break;
        }

        //if (req)
        //    free(req);

        // Mark request as processed.
        io_uring_cqe_seen(&ring, cqe);
    }

    LOG("Closing server socket.");
    close(server_fd);

    return EXIT_SUCCESS;
}
