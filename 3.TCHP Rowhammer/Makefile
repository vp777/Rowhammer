thrh: thrh.c hprh.o
	gcc hprh.o -O0 -std=gnu99 -o "$@" thrh.c 

hprh.o: hprh.c hprh.h
	gcc -O0 -std=gnu99 -c "$<" -o "$@"

clean:
	rm thrh hprh.o
