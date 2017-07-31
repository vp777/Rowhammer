# TC Rowhammer Tool

Tool for testing the rowhammer bug, without need of special permissions (root or CAP_SYS_ADMIN) and Transparent Huge Page (THP) support.
<br><br>
It's based on a [known timing channel](https://people.inf.ethz.ch/omutlu/pub/mph_usenix_security07.pdf) created when different rows with in the same bank are activated .
It is a more targetted approach to rowhammer when no physical mapping information are available. It achieves that by creating a set with Same Bank Different Rows (SBDR) addresses based on the described timing channel. 
<br>

## Command line arguments

<pre>
  -s SIZE               allocate SIZE MB buffer for testing (default is 32 MB)
  -o output_file,       path to the output file, default is stdout
  -m THRESHOLD_MULT     the THRESHOLD_MULT creates the threshold for SBDR and non-SBDR pairs (default is 1.3)
  -i ITERATIONS         gather timings for SBDR pairs after ITERATIONS iterations (default is 5000)
  -q SAMPLE_SIZE        number of measurement to take from a given address pair (default is 8)
  -e SECS               stop the test after SECS seconds (by default stops when all the rows are tested)
</pre>

## Usage

First build the tool by running:

	make
    or
	make debug

Then run it by:

	./sc_rh [-s <buffer_size>] [-o <output_file>] [-m <threshold_mult>] [-i <trial_iterations>] [-q <sample_size>] [-e <run_time>]
    
## Remarks
This tool was tested in both native and virtualized environment and in both cases over 30 bit flips were generated within 30 minutes. Both the host and guest operating system were running Ubuntu 14.04 in a 4-core Sandy Bridge CPU with a single DIMM all running with the default configurations.

## Notes
The debug information (when enabled) about the DRAM mapping are targetted towards Sandy Bridge microarchitecture CPUs. If there is need for stats on a different microarchitecture, the code can be easily extended by using the implementation here: [hammertime](https://github.com/vusec/hammertime/blob/master/ramses/addr.c) <br>
Regardless of the debugging information, the main functionality of the program should stay unaffected regardless of the microarchitecture.<br><br>
It make use of RDTSCP for timing and the CLFLUSH for the cache eviction. In case the RDTSCP is not available, it can be replaced with RDTSC for x86 architectures that support it.
<br><br>
One possible optimization is after identifying the pages that map to the same bank, to try and identify which pages map to the same row. That way, by utilizing full rows, we increase both effectiveness and the efficiency of the attack, especially on microarchitectures that map multiple pages across a single row.