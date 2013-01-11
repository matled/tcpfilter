/*
Copyright (c) 2006, Matthias Lederhofer <matled@gmx.net>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
* Neither the name of the author nor the names of its contributors may be used
  to endorse or promote products derived from this software without specific
  prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/* delay cat - sleep after each read */
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
