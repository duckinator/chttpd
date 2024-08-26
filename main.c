#include <arpa/inet.h>  // htons()
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>      // for printf() and friends
#include <unistd.h>     // for close()
#include <signal.h>
#include <sys/epoll.h>  // for epoll_*
#include <sys/socket.h> // for... socket... stuff...
#include <stdlib.h>     // for EXIT_FAILURE, EXIT_SUCCESS
#include <string.h>     // strlen()
#include <stdbool.h>

#define MAX_EVENTS 10
static int BACKLOG = 50;
static int PORT = 8080;

static int done = false;
void prepare_to_exit(int _signal) {
    if (done) {
        puts("Received Ctrl-C twice; exiting immediately!");
        exit(EXIT_FAILURE);
    }

    puts("Handling remaining connections, then exiting.");
    puts("Press Ctrl-C again to exit immediately.");
    done = true;
}

void register_signal_handler(void) {
    struct sigaction sigact;
    sigact.sa_handler = prepare_to_exit;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, (struct sigaction *)NULL);
}

void watch_socket(int epoll_fd, int sock_fd) {
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = sock_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event)) {
        perror("epoll_ctl");
    }
}

int server_socket(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return -1;
    }

    // Mark address as reusable to avoid problems if client sockets aren't
    // all closed at exit.
    int optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int))) {
        perror("setsockopt");
    }


    if (fcntl(server_fd, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server, sizeof(server))) {
        perror("bind");
        return -1;
    }

    if (listen(server_fd, BACKLOG)) {
        perror("listen");
        return -1;
    }

    return server_fd;
}

void send_response(int fd, char *response) {
    size_t length = strlen(response);
    if (send(fd, response, length, 0) == -1)
        perror("send");
    close(fd);
}

int main(int argc, char *argv[]) {
    for (size_t i = 0; i < argc; i++)
        printf("%s ", argv[i]);
    puts("");

    int epoll_fd = epoll_create1(0);
    puts("Got epoll_fd.");

    if (epoll_fd == -1) {
        perror("epoll_create1");
        return EXIT_FAILURE;
    }

    int server_fd = server_socket();
    if (server_fd == -1) {
        return EXIT_FAILURE;
    }
    puts("Got server socket.");

    register_signal_handler();
    puts("Registered signal handlers.");

    struct epoll_event events[MAX_EVENTS] = {0};
    watch_socket(epoll_fd, server_fd);
    puts("Watching server_fd.");
    while (!done) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (num_events < 0) {
            perror("epoll_wait");
            break;
        }

        for (size_t i = 0; i < num_events; i++) {
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))) {
                perror("epoll_wait");
                close(events[i].data.fd);
                continue;
            }

            if (server_fd == events[i].data.fd) {
                //puts("Handling server socket.");
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

            //puts("Handling client socket.");
            while (true) {
                char buf[513] = {0};
                ssize_t count = read(events[i].data.fd, buf, sizeof buf - 1);
                buf[512] = '\0';

                char *method = NULL;
                char *path = NULL;
                // e.g. "GET <path>"
                int scan_results = sscanf(buf, "%ms %ms", &method, &path);
                if (scan_results == 1) {
                    free(method);
                    method = NULL;
                    scan_results -= 1;
                }

                if (count == EOF) {
                    close(events[i].data.fd);
                    free(method);
                    free(path);
                    break;
                }

                if (count == -1 && errno != EAGAIN) {
                    perror("read");
                    close(events[i].data.fd);
                    free(method);
                    free(path);
                    break;
                }

                if (method && (strncmp(method, "GET", 4) != 0)) {
                    char *response = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 32\r\nAllow: GET\r\n\r\nOnly GET requests are allowed.\r\n";
                    send_response(events[i].data.fd, response);
                    free(method);
                    free(path);
                    close(events[i].data.fd);
                    break;
                }

                if (path) { // if we have the PATH to return.
                    //puts("    Sending response!");
                    char *response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 37\r\nConnection: close\r\n\r\n<!doctype html>\r\n<p>hello, world!</p>";
                    send_response(events[i].data.fd, response);
                }

                free(method);
                free(path);
            } // while (true)
        }
    }

    puts("Closing server socket.");
    close(server_fd);

    puts("Closing epoll socket.");
    close(epoll_fd);

    return EXIT_SUCCESS;
}
