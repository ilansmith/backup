PROG=backup
INSTALL_PATH=/usr/bin

backup: backup.o
	gcc -g -Wall -o $(PROG) $<

backup.o: backup.c
	gcc -g -Wall -o $@ -c $<

install:
	install $(PROG) $(INSTALL_PATH)

uninstall:
	rm -f $(INSTALL_PATH)/$(PROG)

clean:
	rm -rf *.o

cleanall: clean
	rm -rf tags $(PROG)
