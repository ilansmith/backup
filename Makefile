CC=cc
LD=gcc

PROG=backup
INSTALL_PATH=/usr/bin
FILE=CVS/Entries
FILE_EXISTS=[ -n "`stat $(FILE) 2>&1 | grep -i "no such file or directory"`" ]
TAG_STR=$(shell $(FILE_EXISTS) || awk -F/ '/Makefile/ { print $$6 }' $(FILE) | grep Ttag-)
CFLAGS=-g -std=gnu99 -Wall -Werror

ifneq ($(TAG_STR),)
VER=$(shell [ -z "$(TAG_STR)" ] || awk -F/ '/Makefile/ { print $$6 }' $(FILE) |\
	sed 's/Ttag-//' | sed 's/_/./')

DFLAGS+=-DVERSION=$(VER)
endif

ifeq ($(DEBUG),y)
    CFLAGS+=-DDEBUG
endif

$(PROG): backup.o
	$(LD) -o $(PROG) $<

backup.o: backup.c
	$(CC) $(CFLAGS) $(DFLAGS) -o $@ -c $<

install:
	install $(PROG) $(INSTALL_PATH)

uninstall:
	rm -f $(INSTALL_PATH)/$(PROG)

clean:
	rm -rf *.o

cleanall: clean
	rm -rf tags $(PROG)

