PROG=backup
FILE=CVS/Entries
VER=$(shell awk -F/ '/$(PROG)/ { print $$3 }' $(FILE))
CFLAGS=-g -Wall -Werror

backup: backup.o
	gcc -o $(PROG) $<

backup.o: backup.c
	gcc $(CFLAGS) -o $@ -DVERSION=$(VER) -c $<

install:
	cp $(PROG) ~/bin

uninstall:
	rm -f ~/bin/$(PROG)
	
clean:
	rm -rf *.o

cleanall: clean
	rm -rf tags $(PROG)
