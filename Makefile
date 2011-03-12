CC=cc
LD=gcc
CP=cp
RM=rm

PROG=backup
FILE=CVS/Entries
FILE_EXISTS=[ -n "`stat $(FILE) 2>&1 | grep -i "no such file or directory"`" ]
TAG_STR=$(shell $(FILE_EXISTS) || awk -F/ '/Makefile/ { print $$6 }' $(FILE) | grep Ttag-)
CFLAGS=-g -std=gnu99 -Wall -Werror

ifneq ($(TAG_STR),)
VER=$(shell [ -z "$(TAG_STR)" ] || awk -F/ '/Makefile/ { print $$6 }' $(FILE) |\
	sed 's/Ttag-//' | sed 's/_/./')

DFLAGS+=-DVERSION=$(VER)
endif

$(PROG): backup.o
	$(LD) -o $(PROG) $<

backup.o: backup.c
	$(CC) $(CFLAGS) $(DFLAGS) -o $@ -c $<

install:
	$(CP) $(PROG) ~/bin

uninstall:
	$(RM) -f ~/bin/$(PROG)
	
clean:
	$(RM) -rf *.o

cleanall: clean
	$(RM) -rf tags $(PROG)

