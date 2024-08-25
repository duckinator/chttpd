#include <arpa/inet.h>  // htons()
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>      // for printf() and friends
#include <unistd.h>     // for close()
#include <sys/epoll.h>  // for epoll_*
#include <sys/socket.h> // for... socket... stuff...
#include <stdlib.h>     // for EXIT_FAILURE, EXIT_SUCCESS
#include <string.h>     // strlen()
#include <stdbool.h>

#define MAX_EVENTS 10
static int BACKLOG = 50;
static int PORT = 8080;

void watch_socket(int epoll_fd, int sock_fd) {
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = sock_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event)) {
        perror("epoll_ctl");
        //close(epoll_fd);
    }
}

int server_socket() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
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

    if (bind(server_fd, (struct sockaddr*)&server, sizeof(server))) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, BACKLOG)) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}


int main(int argc, char *argv[]) {
    for (size_t i = 0; i < argc; i++)
        printf("%s ", argv[i]);
    puts("");

    int epoll_fd = epoll_create1(0);
    printf("got epoll_fd\n");

    if (epoll_fd == -1) {
        perror("epoll_create1");
        return EXIT_FAILURE;
    }

    int server_fd = server_socket();
    printf("got a server socket\n");

    struct epoll_event events[MAX_EVENTS] = {0};
    watch_socket(epoll_fd, server_fd);
    printf("watching server_fd\n");
    while (true) {
        printf("epoll_wait()...\n");
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (size_t i = 0; i < num_events; i++) {
            printf("Handling event %zu of %i\n", i + 1, num_events);
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))) {
                fprintf(stderr, "!! epoll error\n");
                perror("epoll_wait");
                close(events[i].data.fd);
                continue;
            }

            if (server_fd == events[i].data.fd) {
                printf("Handling server socket.\n");
                while (true) {
                    printf("accept()...\n");
                    int client_fd = accept(server_fd, NULL, NULL);
                    printf("got client_fd: %i\n", client_fd);
                    if (client_fd == -1) {
                        // EAGAIN/EWOULDBLOCK aren't actual errors, so
                        // be quiet about it.
                        //if ((errno != EAGAIN) && (errno != EWOULDBLOCK))
                            perror("accept");
                        break;
                    }

                    watch_socket(epoll_fd, client_fd);
                }
                continue;
            }

            printf("Handling client socket.\n");
            while (true) {
                char buf[512];
                ssize_t count = read(events[i].data.fd, buf, sizeof buf);

                // read() failed
                if (count == -1) {
                    if (errno != EAGAIN) {
                        perror("read");
                        close(events[i].data.fd);
                    }
                    break;
                }

                // there's nothing left to read.
                if (count == 0) {
                    char *response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 37\r\nConnection: close\r\n\r\n<!doctype html>\r\n<p>hello, world!</p>";
                    size_t length = strlen(response);
                    if (send(events[i].data.fd, response, length, 0) == -1)
                        perror("send");
                    close(events[i].data.fd);
                    break;
                }
            } // while (true)
        }
    }

    if (close(epoll_fd)) {
        perror("close(epoll_fd)");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
