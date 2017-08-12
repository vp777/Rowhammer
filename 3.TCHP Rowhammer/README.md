# TCHP Rowhammer Tool

Tool for testing the rowhammer bug, without need of special permissions (root or CAP_SYS_ADMIN) and Transparent Huge Page (THP) support in a more efficient way than [tcrh](https://github.com/vp777/Rowhammer/tree/master/2.TC%20Rowhammer).
<br><br>
This tool tries to identify big chunks in memory that are allocated in physically contiguous space. It does that by utilizing the timing channel created when accessing different rows in the same bank ([tcrh](https://github.com/vp777/Rowhammer/tree/master/2.TC%20Rowhammer)). After the identification phase, that region is passed to the 
[hprh](https://github.com/vp777/Rowhammer/tree/master/1.THP%20Rowhammer) for inducing the rowhammer vulnerability.
<br>

## Operation
For its operation, it basically tries to predict which addresses map to the same bank but different row based on the assumption that the area in which those addresses are lying into is contiguous. Since modern systems have more than 8 banks(so the probability of randomly hitting one is at least 1/8), managing to find such pairs suggests that at least the regions between them could potentially be contiguous.
<br><br>
You can see how linux kernel tends to allocate memory in here: [google-rowhammer](https://github.com/google/rowhammer-test/tree/master/physmem_alloc_analysis).

## Command line arguments

<pre>
  -s SIZE               allocate initially SIZE MB buffer (default is 512 MB)
  -o output_file,       path to the output file, default is stdout
  -m THRESHOLD_MULT     the THRESHOLD_MULT creates the threshold for SBDR and non-SBDR pairs (default is 1.3)
  -i ITERATIONS         gather timings for SBDR pairs after ITERATIONS iterations (default is 5000)
  -b TEST_ITERATIONS    initially hammer the rows for TEST_ITERATIONS iterations (default is 550000)
  -B STRESS_ITERATIONS  after a bit flip is found, hammer again the targets for STRESS_ITERATIONS (default is 1700000)
  -q SAMPLE_SIZE        number of measurement to take from a given address pair (default is 13)
  -e SECS               stop the test after SECS seconds (by default stops when all the rows are tested)
</pre>

## Usage

First build the tool by running one of the following options:

<pre>
make
</pre>

Then run it by:

	./thrh [-s <buffer_size>] [-o <output_file>] [-m <threshold_mult>] [-i <trial_iterations>] [-b <test_iterations>] [-B <stress_iterations>] [-q <sample_size>] [-e <run_time>]
    
## Remarks
This tool was tested in both native and virtualized environment and in some cases it was possible to generate more than 600 bit flips within less than a minute. Both the host and guest (VMware) were running Ubuntu 16.04.2 with the latest kernel version in a Sandy Bridge CPU with a single DIMM all running with the default configurations and THP disabled.

## Notes
The SBDR prediction values are based on the given CPU-DRAM configuration, so for different configurations those values has to be adjusted. For our configuration, those values were generated with [offsetter](https://github.com/vp777/Rowhammer/tree/master/3.TCHP%20Rowhammer/offsets)
<br><br>
As it can be seen from the logs, proper synchronization is needed for optimized results. In this task, the techniques described in [prefetch sidechannel](https://gruss.cc/files/prefetch.pdf) could be useful.