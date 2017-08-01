# Logs generated from the tool

Logs starting with native-* were generated on the host, the vm-* were generated through the guest virtual machine. In both cases the Transparent Huge Pages (THP) were disabled.<br>
The script diff.sh provides some quick stats on the difference between the vulnerable triplet (target row 1, target row 2, victim row)

## Usage

You can run the script with:

	bash diff.sh output.log