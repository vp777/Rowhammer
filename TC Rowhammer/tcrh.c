/*
 * Copyright (c) 2017 Varnavas Papaioannou
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <fcntl.h>
#include <sys/signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

uint64_t BUFSIZE = (1 << 24);
uint64_t MACCESS_ITERATIONS = 5000;
uint64_t SAMPLE_SIZE = 8;
uint64_t CALIBRATION_RUNS = 64;
uint64_t TEST_ITERATIONS = 550000;
uint64_t STRESS_ITERATIONS = 1700000;
float THRESHOLD_MULT = 1.3;

#define PAGE_SIZE 0x1000
#define ROW_GRAN 0x1000 //optional: on sandy bridge, the whole page resides in a single row.

#define MIN(a,b) ((a)>(b)?(b):(a))
#define MAX(a,b) ((a)>(b)?(a):(b))

struct PAGE {
	uintptr_t vaddr;
	uint64_t paddr;
	uint64_t row;
	uint64_t bank;
};

#define MASK(m) ((1ULL<<((m)%(sizeof(1ULL)*8)))-1)
#define GENMASK(l,r) (MASK((l)+1)^MASK(r))
#define BIT(n, x) (((n)>>(x))&1)
#define BITS(n, l, r) (((n)>>(r))&MASK((l)-(r)+1))
#define POP_BIT(n, x) ((((n)>>1)&~MASK(x))|((n)&MASK(x)))

#define rdtscp_begin(t, cycles_high, cycles_low) {							\
	asm volatile ("CPUID\n\t"												\
			"RDTSC\n\t"														\
			"mov %%edx, %0\n\t"												\
			"mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::	\
			"%rax", "%rbx", "%rcx", "%rdx");								\
																			\
	t=((uint64_t) cycles_high << 32) | cycles_low;							\
}

#define rdtscp_end(t, cycles_high1, cycles_low1) {					\
	asm volatile ("RDTSCP\n\t"										\
			"mov %%edx, %0\n\t"										\
			"mov %%eax, %1\n\t"										\
			"CPUID\n\t": "=r" (cycles_high1), "=r" (cycles_low1)::	\
			"%rax", "%rbx", "%rcx", "%rdx");						\
	t=((uint64_t) cycles_high1 << 32) | cycles_low1;				\
}

void flush(uintptr_t vaddr, size_t len) {
	for (uintptr_t caddr = vaddr; caddr < vaddr + len; caddr += 64) {
		asm volatile ("clflush (%0)\n\t": : "r" (caddr) : "memory");
	}
}

void __attribute__((optimize("O1"))) hammer_double (const uintptr_t *addrs, unsigned int itercount) {
	volatile int *p = (volatile int *) (addrs[0]);
	volatile int *q = (volatile int *) (addrs[1]);
	while (itercount--) {
		*p;
		*q;
		asm volatile (
				"clflush (%0)\n\t"
				"clflush (%1)\n\t"
				//"mfence"
				:
				: "r" (p), "r" (q)
				: "memory"
		);
	}
}

uint64_t getPhysAddress(uintptr_t vaddr) {
	uint64_t pmentry;
	static int fd = -1;

	if (fd == -1) {
		fd = open("/proc/self/pagemap", O_RDONLY);
		if(fd==-1){
			perror("pagemap file");
			exit(1);
		}
	}

	if (pread(fd, &pmentry, sizeof(pmentry), vaddr / PAGE_SIZE * sizeof(pmentry)) != sizeof(pmentry))
		return -1;
#if DEBUG==1
	if (!(pmentry & MASK(55))) {
		printf("DEBUG enabled->give me some privileges\n");
		exit(1);
	}
#endif
	return ((pmentry & MASK(55)) << 12) + (vaddr & MASK(12));
}

//DRAM mapping: Sandy Bridge specific
uint64_t getBank(uint64_t paddr) {
	uint64_t bn1, bn2, bank, rnk;
	bn1 = (paddr >> 17) & 7;
	bn2 = (paddr >> 13) & 7;
	bank = bn1 ^ bn2;
	rnk = BIT(paddr, 16) ^ BIT(paddr, 20);

	return (rnk << 3) | (bank);
}

uint64_t getRow(uint64_t paddr) {
	return BITS(paddr, 31, 17);
}

static int u64cmp(const void *p1, const void *p2) {
	const uint64_t v1=*(uint64_t const *) p1;
	const uint64_t v2=*(uint64_t const *) p2;
	return v1>v2?1:(v1==v2?0:-1);
}

size_t sbdr(uintptr_t oaddr, void *mem, size_t len, uintptr_t *set, size_t step) {
	uint32_t tmp1, tmp2;
	uint64_t diffSamples[SAMPLE_SIZE];
	uint64_t start, stop;

	//assuming the data are well distributed across banks, most of the times we will hit different banks (e.g. 15/16)
	uint64_t calSample[CALIBRATION_RUNS];
	size_t i = 0;
	for (uintptr_t buf = (uintptr_t) mem; buf < (uintptr_t) mem + CALIBRATION_RUNS * step; buf += step, i++) {
		const uintptr_t taddr[2] = { oaddr, buf };
		for (int j = 0; j < SAMPLE_SIZE; j++) {
			rdtscp_begin(start, tmp1, tmp2)
			hammer_double(taddr, MACCESS_ITERATIONS);
			rdtscp_end(stop, tmp1, tmp2)
			diffSamples[j] = stop - start;
		}
		qsort(diffSamples, SAMPLE_SIZE, sizeof(uint64_t), u64cmp);

//		uint64_t mdiff = diffSamples[SAMPLE_SIZE / 2];
		uint64_t mdiff = diffSamples[1];
		calSample[i] = mdiff;
	}
	qsort(calSample, CALIBRATION_RUNS, sizeof(uint64_t), u64cmp);


	//run the actual profiling
	uint64_t threshHold = (uint64_t) (calSample[CALIBRATION_RUNS / 2] * THRESHOLD_MULT);
	size_t found = 0;
	for (uintptr_t buf = (uintptr_t) mem; buf < (uintptr_t) mem + len; buf += step) {
		const uintptr_t taddr[2] = { oaddr, buf };
		for (int j = 0; j < SAMPLE_SIZE; j++) {
			rdtscp_begin(start, tmp1, tmp2)
			hammer_double(taddr, MACCESS_ITERATIONS);
			rdtscp_end(stop, tmp1, tmp2)
			diffSamples[j] = stop - start;
		}
		qsort(diffSamples, SAMPLE_SIZE, sizeof(uint64_t), u64cmp);

//		uint64_t mdiff = diffSamples[SAMPLE_SIZE / 2];
		uint64_t mdiff = diffSamples[1];
		if (mdiff > threshHold)
			set[found++] = buf;
	}
	return found;
}

#if DEBUG==1
static int page_row_cmp(const void *p1, const void *p2) {
	const struct PAGE *page1 = (const struct PAGE*) p1;
	const struct PAGE *page2 = (const struct PAGE*) p2;
	return page1->row > page2->row?1:(page1->row == page2->row?0:-1);
}

static void __attribute__((constructor)) checkPageMap() {
	getPhysAddress((uintptr_t) checkPageMap);
}
#endif

void stop_signal(int signal) {
	fflush(NULL);
	exit(0);
}

int main(int argc, char *argv[]) {
	uint32_t *array;
	uintptr_t origin;
	uintptr_t found[BUFSIZE / PAGE_SIZE];
	uint64_t l3cache_size=sysconf(_SC_LEVEL3_CACHE_SIZE);
	uint64_t *evictionBuffer=NULL;
	uint64_t runTime = 0;
	size_t n, i;

	int opt;
	while ((opt = getopt(argc, argv, "s:o:m:i:q:b:B:e:")) != -1) {
		switch (opt) {
		case 's': {
			char *end = NULL;
			BUFSIZE = strtoll(optarg, &end, 0) << (20);
			break;
		}
		case 'o':
			if (!freopen(optarg, "w", stdout)) {
				perror("Cannot write to the given output file:");
			}
			break;
		case 'm': {
			char *end = NULL;
			THRESHOLD_MULT = strtof(optarg, &end);
			break;
		}
		case 'i': {
			char *end = NULL;
			MACCESS_ITERATIONS = strtoll(optarg, &end, 0);
			break;
		}
		case 'q': {
				char *end = NULL;
				SAMPLE_SIZE = strtoll(optarg, &end, 0);
				if(SAMPLE_SIZE<2) {
					fprintf(stderr, "Too small sample size, minimum is 4\n");
					SAMPLE_SIZE=4;
				}
				break;
			}
		case 'b': {
			char *end = NULL;
			TEST_ITERATIONS = strtoll(optarg, &end, 0);
			break;
		}
		case 'B': {
			char *end = NULL;
			STRESS_ITERATIONS = strtoll(optarg, &end, 0);
			break;
		}
		case 'e': {
			char *end = NULL;
			runTime = strtoll(optarg, &end, 0);
			break;
		}
		default:
			fprintf(stderr, "Invalid argument %c\n", opt);
			return -1;
		}
	}


	array = mmap(NULL, BUFSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	memset((void*) array, 0xff, BUFSIZE);

	origin = (uintptr_t) array;
	n = sbdr(origin, array, BUFSIZE, found, PAGE_SIZE);

	if((uint64_t)n*PAGE_SIZE>(uint64_t)(2.0*l3cache_size)){//maybe to much work that way
		fprintf(stderr, "[!]Using eviction buffer\n");
		evictionBuffer=(uint64_t*)malloc(l3cache_size);//just evict the whole l3 cache
	}
	fprintf(stderr, "Found in bank=%zu\n", n);

	uint8_t banksNum[]={8,16,32,64};
	for(i=0;i<sizeof(banksNum)/sizeof(*banksNum);i++){
		uint64_t lrange=(size_t)(0.85*BUFSIZE / ROW_GRAN / banksNum[i]);
		uint64_t urange=(size_t)(1.15*BUFSIZE / ROW_GRAN / banksNum[i]);
		if(n>lrange && n<urange) break;
	}

	if(i==sizeof(banksNum)/sizeof(*banksNum)) {
		fprintf(stderr, "[!]Not the expected number of addresses in the bank, you may want to try again\n");
		sleep(5);
	} else {
		fprintf(stderr, "[!]I think we have a total of %u banks\n", banksNum[i]);
		sleep(3);
	}

	struct PAGE pages[n];
	for (size_t i = 0; i < n; i++) {
		pages[i].vaddr = found[i];
#if DEBUG==1
		pages[i].paddr = getPhysAddress(found[i]);
		pages[i].row = getRow(pages[i].paddr);
		pages[i].bank = getBank(pages[i].paddr);
#endif
	}

#if DEBUG==1
	{
		uint64_t originBank = getBank(getPhysAddress(origin));
		size_t err = 0;
		for (size_t i = 0; i < n; i++) {
			if (pages[i].bank != originBank)
				err++;
			fprintf(stderr, "Bank=%lu Row=%lu addr=0x%012lx phys=0x%012lx\n", pages[i].bank, pages[i].row, pages[i].vaddr, pages[i].paddr);
		}

		if (err > 0) {
			fprintf(stderr, "Found %zu possible misplacements in the bank\n", err);
		} else {
			fprintf(stderr, "Map seems to be accurate\n");
		}

		sleep(3);
		qsort(pages, n, sizeof(struct PAGE), page_row_cmp);
	}
#endif
	if (runTime > 0) {
		signal(SIGALRM, stop_signal);
		alarm(runTime);
	}
	signal(SIGINT, stop_signal);

	for (size_t i = 0; i < n; i++) {
		if (i%10==0) fflush(stdout);
		printf("[%zu]Testing %012lx\n", i, pages[i].vaddr);
#if DEBUG==1
		{
			//check if the row is already tested
			if (i > 0 && pages[i - 1].row == pages[i].row)
				continue;

			//try to find triplets for double sided rowhammer
			size_t m=i+1;
			while(m<n && pages[m].row==pages[i].row && m++);
			if(m==n || pages[m].row!=pages[i].row+1) continue;
			size_t j=m++;
			while(m<n && pages[m].row==pages[j].row && m++);
			if(m==n || pages[m].row!=pages[i].row+2) continue;

			printf("Indexes (%zu=%lu, %zu=%lu, %zu=%lu)\n",
					i, pages[i].row,
					m, pages[m].row,
					j, pages[j].row);

			printf("........................\n");
		}
#endif
		//prepare the first target row for hammering
		uintptr_t dside = pages[i].vaddr;
		memset((void*) dside, 0x00, PAGE_SIZE);
		flush(dside, PAGE_SIZE);
		for (int j = i + 1; j < n; j++) {
			//prepare the other target row for hammering
			uintptr_t uside = pages[j].vaddr;
			memset((void*) uside, 0x00, PAGE_SIZE);
			flush(uside, PAGE_SIZE);

			const uintptr_t taddr[2] = { dside, uside };
			hammer_double(taddr, TEST_ITERATIONS);

			//Now flush the l3 cache and check if we have any victims
			int externalRun=0;

			if(evictionBuffer){
				uint64_t sum=0;
				for(size_t i=0; i < l3cache_size/sizeof(uint64_t); i++){
					sum+=evictionBuffer[i];
				}
			}else{
				for(size_t i = 0; i < n; i++)
					flush(pages[i].vaddr, PAGE_SIZE);
			}

			flip_check_label:
			for (size_t pageO = 0; pageO < n; pageO++) {
				int gotBitFlips=0;

				if (pages[pageO].vaddr == dside || pages[pageO].vaddr == uside)
					continue;

				for (size_t offset = 0; offset < PAGE_SIZE; offset++) {
					uint8_t got = *(volatile uint8_t*) (pages[pageO].vaddr + offset);

					if (got != 0xff) {
						if(!externalRun){
							printf("++++++++++++++++++++\n");
							hammer_double(taddr, STRESS_ITERATIONS);
							sleep(2);
							hammer_double(taddr, STRESS_ITERATIONS);
							sleep(2);

							externalRun++;
							goto flip_check_label;
						}

						if(!gotBitFlips) gotBitFlips++;

#if DEBUG==1
						printf("(^0x%012lx=0x%012lx,^0x%012lx=0x%012lx) Found [0x%012lx=0x%012lx] bitflip [%zu]=0x%02x (^%lu,%lu,^%lu)\n",
								dside, pages[i].paddr,
								uside, pages[j].paddr,
								pages[pageO].vaddr, pages[pageO].paddr,
								offset,
								got & 0xff,
								pages[i].row, pages[pageO].row, pages[j].row);
#else
						printf("(0x%012lx,0x%012lx) Found [0x%012lx] bitflip [%zu]=0x%02x\n",
								dside, uside,
								pages[pageO].vaddr,
								offset,
								got&0xff);
#endif
						*(volatile uint8_t*) (pages[pageO].vaddr + offset) = 0xff;
					}
				}
				if(gotBitFlips) {
					flush(pages[pageO].vaddr, PAGE_SIZE);
					printf("---------------------\n");
				}
			}
			memset((void*) uside, 0xff, PAGE_SIZE);
			flush(uside, PAGE_SIZE);
		}
		memset((void*) dside, 0xff, PAGE_SIZE);
		flush(dside, PAGE_SIZE);
	}
	return 0;
}
