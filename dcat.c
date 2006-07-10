/* (c) 2006 Matthias Lederhofer
 * delay cat - sleep after each read
 */
#define _BSD_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>

#define BUFSIZE 1024

int main(int argc, char **argv)
{
    if (argc != 2)
        errx(EXIT_FAILURE, "give delay as argument");
    unsigned long delay;
    delay = strtoul(argv[1], NULL, 10);
    if (delay == ULONG_MAX)
        err(EXIT_FAILURE, "invalid value for delay");
    delay *= 1000;

    char buf[BUFSIZE];
    ssize_t pos = 0;
    ssize_t len = 0;
    for (;;) {
        if (len) {
            ssize_t n = write(STDOUT_FILENO, buf+pos, len);
            if (n == -1 && errno == EINTR)
                continue;
            if (n == 0)
                exit(EXIT_SUCCESS);
            if (n == -1)
                exit(EXIT_FAILURE);
            len -= n;
            pos += n;
        } else {
            len = read(STDIN_FILENO, buf, BUFSIZE);
            pos = 0;
            if (len == -1 && errno == EINTR)
                continue;
            if (len == 0)
                exit(EXIT_SUCCESS);
            if (len == -1)
                exit(EXIT_FAILURE);
            usleep(delay);
        }
    }
}
