# offsetter

The tool used to generate the distance between addresses that map in different row but in the same bank.

## Usage

First build the tool by executing:

	gcc offsetter.c -o offsetter

Then run it by:

	./offsetter

## Notes
The proper mapping functions have to be set in order to generate the proper results. By default it will generate the differences in Sandy Bridge for a single DIMM in a single channel. Check [hammertime](https://github.com/vusec/hammertime/) for some example implementations.