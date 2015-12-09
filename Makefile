PROG=backup

backup: backup.o
	gcc -g -Wall -o $(PROG) $<

backup.o: backup.c
	gcc -g -Wall -o $@ -c $<

clean:
	rm -rf *.o

cleanall: clean
	rm -rf tags $(PROG)
