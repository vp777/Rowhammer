# Logs generated from the tool

Logs starting with native-* were generated on the host, the vm-* were generated through the guest virtual machine. In both cases the Transparent Huge Pages (THP) were disabled.<br>
The script diff.sh provides some quick stats on the difference between the vulnerable triplet (target row 1, target row 2, victim row)

The logs were generated with:

	./tcrh<-ext> -s 16 -o vm<-ext>-16mb-20min-1.log -e $((20*60))

You can run the script with:

	bash diff.sh output.log
	
Count the number of bit flips:

	fgrep tfl vm-16mb-20min-1.log | wc -l

Total number of unique bit flips per victim row:

	cut -d" " -f3,5 vm-16mb-20min-1.log | sort | uniq | wc -l

