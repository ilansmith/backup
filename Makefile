PROG=backup
INSTALL_PATH=/usr/bin
FILE=CVS/Entries
VER=$(shell awk -F/ '/$(PROG)/ { print $$3 }' $(FILE))
CFLAGS=-g -Wall -Werror

backup: backup.o
	gcc -o $(PROG) $<

backup.o: backup.c
	gcc $(CFLAGS) -o $@ -DVERSION=$(VER) -c $<

install:
	install $(PROG) $(INSTALL_PATH)

uninstall:
	rm -f $(INSTALL_PATH)/$(PROG)

clean:
	rm -rf *.o

cleanall: clean
	rm -rf tags $(PROG)
