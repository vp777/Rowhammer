tcrh: tcrh.c
	gcc -O0 -std=gnu99 -Wall -o tcrh tcrh.c

ext: tcrh_ext.c
	gcc -O0 -std=gnu99 -Wall -o tcrh_ext tcrh_ext.c

debug: tcrh.c tcrh_ext.c
	gcc -O0 -std=gnu99 -DDEBUG=1 -Wall -o tcrh tcrh.c
	gcc -O0 -std=gnu99 -DDEBUG=1 -Wall -o tcrh_ext tcrh_ext.c

clean:
	rm tcrh tcrh_ext 
