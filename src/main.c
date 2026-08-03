#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "event_loop.h"

#define PORT 8080
#define BACKLOG 128 /* completed connections queued for accept(); does not bound in-flight SYNs */

static int create_listen_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Without this, restarting the server right after it exits fails with
     * EADDRINUSE while the previous socket sits in TIME_WAIT. */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(PORT),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        exit(EXIT_FAILURE);
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        exit(EXIT_FAILURE);
    }

    return fd;
}

int main(void) {
    int listen_fd = create_listen_socket();
    printf("Servidor escutando na porta %d (epoll, edge-triggered)...\n", PORT);

    int status = event_loop_run(listen_fd);

    close(listen_fd);
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
