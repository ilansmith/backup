CC=cc
LD=gcc

PROG=backup
INSTALL_PATH=/usr/bin
CFLAGS=-std=gnu99 -Wall -Werror
VER=$(shell git describe --tag | sed 's/tag-//g')

ifeq ($(VER),)
VER=N/A
endif

CFLAGS+=-DVERSION=\"$(VER)\"

ifeq ($(DEBUG),y)
    CFLAGS+=-g -DDEBUG
else
    CFLAGS+=-O3
endif

$(PROG): backup.o
	$(LD) -o $(PROG) $<

backup.o: backup.c
	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	rm -rf *.o

cleanall: clean
	rm -rf tags $(PROG)

