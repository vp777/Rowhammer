# Some logs generated from the tool

The logs were generated with:

	./hprh -s 2 -o native-2mb-1.log

Count the number of (unique) bit flips:

	fgrep tfl native-2mb-1.log | wc -l

## Note:

Transparent Huge Pages (THP) were enabled in both runs