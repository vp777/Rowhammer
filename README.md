# Rowhammer Attack

This repository contains various tools for testing the Rowhammer vulnerability in userspace.

## 0. Extensions
Patches for [Transparent Huge Pages (THP)](https://www.kernel.org/doc/Documentation/vm/transhuge.txt) support in [hammertime](https://github.com/vusec/hammertime/) and [rowhammer-test](https://github.com/google/rowhammer-test/).

## 1. THP Rowhammer (hprh)
Standalone tool that is based on Transparent Huge Pages (THP) feature, a feature that is by default enabled in various Linux distributions

## 2. TC Rowhammer (tcrh)
tcrh utilizes a [timing channel](https://people.inf.ethz.ch/omutlu/pub/mph_usenix_security07.pdf) to identify possible targets that are mapped within the same bank. After the identification phase it exhaustively tests this limited set of addresses for the rowhammer vulnerability. It does not depend on the THP feature but is significantly slower than hprh.

## 3. TCHP Rowhammer (thrh)
Finally, thrh makes use of the timing channel used in tcrh to identify regions in memory that are physically contiguous. When a region is identified, it is passed to hprh to induce the vulnerability. This tools is based on the Linux kernel allocation patterns and it has the potential to work as efficiently as hprh without the need of THP.

## Remarks
The provided tools are cabable of inducing the Rowhammer vulnerability without special privileges and as such the techniques described are practical for use in actual exploitation scenarios ([example](https://www.blackhat.com/docs/us-15/materials/us-15-Seaborn-Exploiting-The-DRAM-Rowhammer-Bug-To-Gain-Kernel-Privileges.pdf)).
<br>
<br>
If there is interest of just exploring and testing for the vulnerability, I would suggest to check out [hammertime](https://github.com/vusec/hammertime/). They provide various features for experimenting with the vulnerability with support for various microarchitectures and configurations.