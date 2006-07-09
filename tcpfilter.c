/* (c) 2006 Matthias Lederhofer <matled@gmx.net> */
/* include {{{ */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
/* }}} */

#define die(...) err(EXIT_FAILURE, __VA_ARGS__)

#define LISTEN_IP "0.0.0.0"
#define LISTEN_PORT 54321
#define REMOTE_IP "10.66.1.10"
#define REMOTE_PORT 80
#define FILTER_IN "cat"
#define FILTER_OUT "echo '==> start <=='; tr o 0; echo '==> end <=='"
#define BUF_SIZE 1024

const char *colortable[] =
/*{{{*/ {
    "7;32", /* general */
    "7;31", /* client -> in filter */
    "7;33", /* in filter -> server */
    "7;36", /* server -> out filter */
    "7;34", /* out filter -> client */
}; /*}}}*/

struct pipe_t
/*{{{*/ {
    int infd;
    int outfd;
    char buf[BUF_SIZE];
    ssize_t len;
    ssize_t pos;
    unsigned dead : 1;
}; /*}}}*/

static void handle_client(struct sockaddr_in*, int);
static void logging(struct sockaddr_in*, int id, const char*, ...);
static void logtraffic(struct sockaddr_in *, int, const char *, ssize_t);
static void filter(const char *cmd, int in, int out);
static void sig_child(int);

int main(int argc, char **argv)
/*{{{*/ {
    if (signal(SIGCHLD, sig_child)) die("signal()");
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) die("socket()");
    {
        int tmp = 1;
        if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof(tmp)) == -1)
            die("setsockopt(SO_REUSEADDR)");
    }
    {
        struct sockaddr_in tmp;
        memset(&tmp, '\0', sizeof(tmp));
        tmp.sin_family = AF_INET;
        tmp.sin_port = htons(LISTEN_PORT);
        tmp.sin_addr.s_addr = inet_addr(LISTEN_IP);
        if (bind(listenfd, (struct sockaddr*)&tmp, sizeof(tmp)) == -1) die("bind()");
    }
    if (listen(listenfd, 0) == -1) die("listen()");

    struct sockaddr_in clientaddr;
    socklen_t clientaddr_len;
    for (;;) {
        clientaddr_len = sizeof(clientaddr);
        int clientfd = accept(listenfd, (struct sockaddr*)&clientaddr,
            &clientaddr_len);
        if (clientfd == -1) {
            if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK ||
                errno == EOPNOTSUPP || errno == EFAULT)
                die("accept()");
            if (errno != EINTR)
                warn("accept()");
            continue;
        }

        switch (fork()) {
            case 0:
                close(listenfd);
                handle_client(&clientaddr, clientfd);
                exit(EXIT_SUCCESS);
            case -1:
                warn("fork()");
        }
        close(clientfd);
    }
} /*}}}*/

static void handle_client(struct sockaddr_in *addr, int client)
/*{{{*/ {
    logging(addr, -1, "new connection");

    int server = socket(PF_INET, SOCK_STREAM, 0);
    if (server == -1) die("socket()");
    {
        struct sockaddr_in tmp;
        memset(&tmp, '\0', sizeof(tmp));
        tmp.sin_family = AF_INET;
        tmp.sin_port = htons(REMOTE_PORT);
        tmp.sin_addr.s_addr = inet_addr(REMOTE_IP);
        if (connect(server, (struct sockaddr*)&tmp, sizeof(tmp)) == -1) die("connect()");
    }

    struct pipe_t pipes[4];
    pipes[0].infd = client;
    pipes[1].outfd = server;
    pipes[2].infd = server;
    pipes[3].outfd = client;
    for (int i = 0; i < 4; ++i) {
        pipes[i].len = 0;
        pipes[i].dead = 0;
    }

    /* start filter programs {{{ */
    {
        int fd[2][2];
        if (pipe(fd[0]) == -1) die("pipe()");
        if (pipe(fd[1]) == -1) die("pipe()");
        pipes[0].outfd = fd[0][1];
        pipes[1].infd =  fd[1][0];
        switch (fork()) {
            case 0:
                close(client);
                close(server);
                close(fd[0][1]);
                close(fd[1][0]);
                filter(FILTER_IN, fd[0][0], fd[1][1]);
                exit(EXIT_SUCCESS);
            case -1:
                die("fork()");
        }
        close(fd[0][0]);
        close(fd[1][1]);
    }
    {
        int fd[2][2];
        if (pipe(fd[0]) == -1) die("pipe()");
        if (pipe(fd[1]) == -1) die("pipe()");
        pipes[2].outfd = fd[0][1];
        pipes[3].infd =  fd[1][0];
        switch (fork()) {
            case 0:
                close(client);
                close(server);
                close(fd[0][1]);
                close(fd[1][0]);
                filter(FILTER_OUT, fd[0][0], fd[1][1]);
                exit(EXIT_SUCCESS);
            case -1:
                die("fork()");
        }
        close(fd[0][0]);
        close(fd[1][1]);
    }
    /* }}} */

    /* pipes:
     * 0: client -> in
     * 1: in     -> server
     * 2: server -> out
     * 3: out    -> client
     */

    for (;;) {
        fd_set rfds, wfds;
        int max = -1;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        for (int i = 0; i < 4; ++i) {
            if (pipes[i].dead) continue;
            if (pipes[i].len == 0) {
                FD_SET(pipes[i].infd, &rfds);
                max = pipes[i].infd > max ? pipes[i].infd : max;
            } else {
                FD_SET(pipes[i].outfd, &wfds);
                max = pipes[i].outfd > max ? pipes[i].outfd : max;
            }
        }
        if (max == -1) break;
        if (select(max+1, &rfds, &wfds, NULL, NULL) == -1) {
            if (errno == EINTR) continue;
            die("select()");
        }
        for (int i = 0; i < 4; ++i) {
            if (FD_ISSET(pipes[i].infd, &rfds)) {
                pipes[i].len = read(pipes[i].infd, pipes[i].buf, BUF_SIZE);
                pipes[i].pos = 0;
                if (pipes[i].len == -1 && errno == EINTR) continue;
                if (pipes[i].len == 0 || pipes[i].len == -1) {
                    /* client/out filter */
                    if (i == 0 || i == 3)
                        goto end;
                    /* in filter/server */
                    if (i == 1 || i == 2) {
                        pipes[1].dead = 1;
                        close(pipes[1].infd);
                        pipes[2].dead = 1;
                        close(pipes[2].infd);
                        close(pipes[2].outfd);
                    }
                    continue;
                }
                logtraffic(addr, i, pipes[i].buf, pipes[i].len);
            } else if (FD_ISSET(pipes[i].outfd, &wfds)) {
                ssize_t n = write(pipes[i].outfd, pipes[i].buf+pipes[i].pos,
                    pipes[i].len);
                if (n == -1 && errno == EINTR) continue;
                if (n == 0 || n == -1) {
                    /* in filter/client */
                    if (i == 0 || i == 3)
                        goto end;
                    /* server/out filter */
                    if (i == 1 || i == 2) {
                        pipes[1].dead = 1;
                        close(pipes[1].infd);
                        pipes[2].dead = 1;
                        close(pipes[2].infd);
                        close(pipes[2].outfd);
                    }
                    continue;
                }
                if (n > pipes[i].len) {
                    warnx("wrote more than expected");
                    pipes[i].len = 0;
                }
                pipes[i].len -= n;
                pipes[i].pos += n;
            }
        }
    }
    end:
    for (int i = 0; i < 4; ++i) {
        close(pipes[i].infd);
        close(pipes[i].outfd);
    }
    logging(addr, -1, "end");
} /*}}}*/

static void filter(const char *cmd, int in, int out)
/*{{{*/ {
    close(0);
    close(1);
    if (dup2(in, 0) != 0) die("dup2()");
    if (dup2(out, 1) != 1) die("dup2()");
    close(in);
    close(out);
    execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
    die("execl()");
} /*}}}*/

static void logging(struct sockaddr_in *addr, int id, const char *fmt, ...)
/*{{{*/ {
    printf("\x1b[%sm===>\x1b[0m ", colortable[id+1]);
    printf("%d:%s:%d%s", getpid(),
        inet_ntoa(addr->sin_addr), ntohs(addr->sin_port),
        fmt == NULL ? "" : " ");
    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    printf(" \x1b[%sm<===\x1b[0m\n", colortable[id+1]);
} /*}}}*/

static void logtraffic(struct sockaddr_in *addr, int id, const char *buf, ssize_t len)
/*{{{*/ {
    logging(addr, id, NULL);
    if (len == 0) return;
    int space = 0;
    while (len--) {
        if (*buf == '\n' && space) {
            printf("\x1b[34m$\x1b[0m");
        }
        if ((*buf < 0x20 && *buf != '\n') || *buf == 0x7F) {
            if (*buf == '\r') {
                printf("\x1b[31m%s\x1b[0m", "\\r");
            } else {
                printf("\x1b[1;31m\\x%02X\x1b[0m", *buf & 0xFF);
            }
        } else {
            putchar(*buf);
        }
        space = (*buf == 0x20);
        ++buf;
    }
    if (*(buf-1) != '\n') {
        printf("\x1b[34m$$\x1b[0m\n");
    }
} /*}}}*/

void sig_child(int i)
/*{{{*/ {
    i = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = i;
    return;
} /*}}}*/
