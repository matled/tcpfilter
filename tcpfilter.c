/* (c) 2006 Matthias Lederhofer <matled@gmx.net> */
/* include {{{ */
#define _BSD_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/md5.h>
/* }}} */

#define die(...) err(EXIT_FAILURE, __VA_ARGS__)
#define diex(...) errx(EXIT_FAILURE, __VA_ARGS__)

#define LOG_CON_NEW 4
#define LOG_CON_END 5
#define DEFAULT_LISTEN_ADDR "0.0.0.0"
#define DEFAULT_FILTER_IN "cat"
#define DEFAULT_FILTER_OUT "cat"
#define BUF_SIZE 1024

static struct {
    struct sockaddr_in listen;
    struct sockaddr_in remote;
    const char *filter_in;
    const char *filter_out;
} global;

static const char *colortable[] =
/*{{{*/ {
    "7;31", /* client -> in filter */
    "1;31", /* in filter -> server */
    "1;32", /* server -> out filter */
    "7;32", /* out filter -> client */
    "7;34", /* new connection */
    "7;36", /* end of connection */
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
static void logging(struct sockaddr_in*, int, const char*, ...);
static void logtraffic(struct sockaddr_in *, int, const char*, ssize_t);
static void filter(const char*, int, int);
static void sig_child(int);
static void arguments(int, char**);
static void usage(void);

int main(int argc, char **argv)
/*{{{*/ {
    arguments(argc, argv);
    
    if (signal(SIGCHLD, sig_child)) die("signal()");
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) die("socket()");
    {
        int tmp = 1;
        if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                &tmp, sizeof(tmp)) == -1)
            die("setsockopt(SO_REUSEADDR)");
    }
    if (bind(listenfd, (struct sockaddr*)&global.listen,
            sizeof(global.listen)) == -1)
        die("bind()");
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
    logging(addr, LOG_CON_NEW, "new connection");

    int server = socket(PF_INET, SOCK_STREAM, 0);
    if (server == -1) die("socket()");
    if (connect(server, (struct sockaddr*)&global.remote,
            sizeof(global.remote)) == -1)
        die("connect()");

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
                filter(global.filter_in, fd[0][0], fd[1][1]);
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
                filter(global.filter_out, fd[0][0], fd[1][1]);
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
                /* read from client/server */
                if (!(i & 1))
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
                /* write to client/server */
                if (i & 1)
                    logtraffic(addr, i, pipes[i].buf+pipes[i].pos,
                        pipes[i].len);
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
    logging(addr, LOG_CON_END, "end");
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
    struct timeval tv;
    struct tm st;
    if (gettimeofday(&tv, NULL) == -1)
        die("gettimeofday()");
    if (localtime_r(&tv.tv_sec, &st) == NULL)
        die("localtime_r()");
    printf("\x1b[%sm==>\x1b[0m ", colortable[id]);
    printf("%04d-%02d-%02d %02d:%02d:%02d.%06u %d:%s:%d%s",
        st.tm_year+1900, st.tm_mon, st.tm_mday,
        st.tm_hour, st.tm_min, st.tm_sec, tv.tv_usec,
        getpid(), inet_ntoa(addr->sin_addr), ntohs(addr->sin_port),
        fmt == NULL ? "" : " ");
    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    printf(" \x1b[%sm<==\x1b[0m\n", colortable[id]);
} /*}}}*/

static void logtraffic(struct sockaddr_in *addr, int id,
    const char *buf, ssize_t len)
/*{{{*/ {
    static unsigned char *lasthash = NULL;
    static unsigned char *newhash = NULL;
    if (lasthash == NULL || newhash == NULL) {
        lasthash = malloc(2*MD5_DIGEST_LENGTH);
        if (lasthash == NULL)
            die("malloc()");
        newhash = lasthash+MD5_DIGEST_LENGTH;
        memset(lasthash, '\0', 2*MD5_DIGEST_LENGTH);
        MD5((const unsigned char*)buf, len, lasthash);
    } else {
        MD5((const unsigned char*)buf, len, newhash);
        unsigned char *tmp = newhash;
        newhash = lasthash;
        lasthash = tmp;
        if (memcmp(lasthash, newhash, MD5_DIGEST_LENGTH) == 0) {
            logging(addr, id, "COPY");
            return;
        }
    }

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

static void arguments(int argc, char **argv)
/*{{{*/ {
    int listen_port = -1;

    memset(&global.listen, '\0', sizeof(global.listen));
    global.listen.sin_family = AF_INET;
    memset(&global.remote, '\0', sizeof(global.remote));
    global.remote.sin_family = AF_INET;

    global.filter_in = DEFAULT_FILTER_IN;
    global.filter_out = DEFAULT_FILTER_OUT;
    if (!inet_aton(DEFAULT_LISTEN_ADDR, &global.listen.sin_addr))
        diex("Argh! Invalid default listen address.");

    for(;;) {
        switch (getopt(argc, argv, "a:p:i:o:")) {
            case 'a':
                if (!inet_aton(optarg, &global.listen.sin_addr))
                    diex("invalid listen address");
                break;
            case 'p':
                listen_port = atoi(optarg);
                if (listen_port & ~0xFFFF || listen_port == 0)
                    diex("invalid listen port");
                global.listen.sin_port = htons(listen_port);
                break;
            case 'i':
                global.filter_in = optarg;
                break;
            case 'o':
                global.filter_out = optarg;
                break;
            case -1:
                goto arguments_getopt_end;
            default:
                usage();
        }
    }
    arguments_getopt_end:
    if (argc-optind != 2)
        usage();
    if (!inet_aton(argv[optind++], &global.remote.sin_addr))
        diex("invalid remote address");
    int remote_port = atoi(argv[optind++]);
    if (remote_port & ~0xFFFF || remote_port == 0)
        diex("invalid remote port");
    global.remote.sin_port = htons(remote_port);
    if (listen_port == -1)
        global.listen.sin_port = global.remote.sin_port;
} /*}}}*/

static void usage()
/*{{{*/ {
    printf("Usage: %s [OPTIONS] <remote address> <remote port>\n"
        "    -a <bind address>\n"
        "    -p <listen port>\n"
        "    -i <in filter>\n"
        "    -o <out filter>\n"
        /* TODO: argv[0] */
        , "tcpfilter");
    exit(EXIT_SUCCESS);
} /*}}}*/
