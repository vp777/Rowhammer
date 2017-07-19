# Rowhammer Testing Program

Tool for testing the rowhammer bug without requiring root privileges.
<br>
Based on the Transparent Huge Pages(THP).
<br>
It is parametrised and it should run for most of the configurations that are based on Sandy Bridge microarchitecture.
<br>

<hr>

## Command line arguments

<pre>
  -s SIZE               allocate SIZE MB buffer for testing (default is 2 MB)
  -o output_file,       path to the output file, default is stdout
  -c CHANNELS           number of active channels in motherboard (default is 1)
  -d DIMMS              number of dimms per channel, 0=disabled,1=enabled (default is 1)
  -r RANKS              number of ranks per dimm (default is 2)
  -m MIRRORING          enable/disable rank mirroring (default is 1)
  -t TPATT              use TPATT for the target rows (default is 0xff)
  -v VPATT              use VPATT for the victim rows (default is 0x00)
  -e SECS               stop testing after SECS seconds (by default stops when all the rows are tested)
</pre>

<br>
<br>

## Using the tool

First build the tool by running:

	make

Then run it by:

	./hp_rh
    
By default, it should be able to identify vulnerable modules in a single channel, single dimm, dual rank configurations.
