PROG=backup
INSTALL_PATH=/usr/bin
FILE=CVS/Entries
TAG_STR=$(shell awk -F/ '/Makefile/ { print $6 }' CVS/Entries | grep Ttag-)
CFLAGS=-g -Wall -Werror

ifneq ($(TAG_STR),)
VER=$(shell [ -z "$(TAG_STR)" ] || awk -F/ '/Makefile/ { print $$6 }' $(FILE) |\
	sed 's/Ttag-//' | sed 's/_/./')

DFLAGS+=-DVERSION=$(VER)
endif

backup: backup.o
	gcc -o $(PROG) $<

backup.o: backup.c
	gcc $(CFLAGS) $(DFLAGS) -o $@ -c $<

install:
	install $(PROG) $(INSTALL_PATH)

uninstall:
	rm -f $(INSTALL_PATH)/$(PROG)

clean:
	rm -rf *.o

cleanall: clean
	rm -rf tags $(PROG)
