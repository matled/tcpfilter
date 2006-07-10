-include Makefile.local

CFLAGS+=-std=c99 #-pedantic-errors

ifneq ($(DEBUG),)
        CFLAGS+=-ggdb3
        CFLAGS+=-Wall -W -Wchar-subscripts -Wmissing-prototypes
        CFLAGS+=-Wmissing-declarations -Wredundant-decls
        CFLAGS+=-Wstrict-prototypes -Wshadow -Wbad-function-cast
        CFLAGS+=-Winline -Wpointer-arith -Wsign-compare
        CFLAGS+=-Wunreachable-code -Wdisabled-optimization
        CFLAGS+=-Wcast-align -Wwrite-strings -Wnested-externs -Wundef
        CFLAGS+=-DDEBUG
        LDFLAGS+=
else
        CFLAGS+=-O2
        LDFLAGS+=
endif

all: tcpfilter dcat
clean:
	rm -f tcpfilter dcat
.PHONY: all clean

tcpfilter: LDFLAGS+=-lssl
