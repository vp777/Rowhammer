hprh: hprh.h hprh.c
	gcc -O2 -std=gnu99 -fPIC -o "$@" hprh.c

clean:
	rm hprh 
