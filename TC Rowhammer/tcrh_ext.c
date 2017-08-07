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
#define ROW_LEN (1<<13)
#define PAGES_PER_ROW (ROW_LEN/ROW_GRAN)

#define MIN(a,b) ((a)>(b)?(b):(a))
#define MAX(a,b) ((a)>(b)?(a):(b))

struct DRAM_ROW {
	uint64_t drid;
	uintptr_t vaddr[PAGES_PER_ROW];
	size_t dplen[PAGES_PER_ROW];
	size_t size;
	uint64_t paddr;
	uint64_t row;
	uint64_t bank;
};

#define MASK(m) ((1ULL<<((m)%(sizeof(1ULL)*8)))-1)
#define GENMASK(l,r) (MASK((l)+1)^MASK(r))
#define BIT(n, x) (((n)>>(x))&1)
#define BITS(n, l, r) (((n)>>(r))&MASK((l)-(r)+1))
#define POP_BIT(n, x) ((((n)>>1)&~MASK(x))|((n)&MASK(x)))

#define rdtscp_begin(t, cycles_high, cycles_low) do {						\
	asm volatile ("CPUID\n\t"												\
			"RDTSC\n\t"														\
			"mov %%edx, %0\n\t"												\
			"mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::	\
			"%rax", "%rbx", "%rcx", "%rdx");								\
																			\
	t=((uint64_t) cycles_high << 32) | cycles_low;							\
} while (0)

#define rdtscp_end(t, cycles_high1, cycles_low1) do {					\
	asm volatile ("RDTSCP\n\t"										\
			"mov %%edx, %0\n\t"										\
			"mov %%eax, %1\n\t"										\
			"CPUID\n\t": "=r" (cycles_high1), "=r" (cycles_low1)::	\
			"%rax", "%rbx", "%rcx", "%rdx");						\
	t=((uint64_t) cycles_high1 << 32) | cycles_low1;				\
} while (0)

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

void __attribute__((optimize("O1"))) hammer_single (const uintptr_t addr, unsigned int itercount) {
	volatile int *p = (volatile int *) (addr);

	while (itercount--) {
		*p;
		asm volatile (
				"clflush (%0)\n\t"
				//"mfence"
				:
				: "r" (p)
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
			rdtscp_begin(start, tmp1, tmp2);
			hammer_double(taddr, MACCESS_ITERATIONS);
			rdtscp_end(stop, tmp1, tmp2);
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
			rdtscp_begin(start, tmp1, tmp2);
			hammer_double(taddr, MACCESS_ITERATIONS);
			rdtscp_end(stop, tmp1, tmp2);
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

size_t sr(uintptr_t oaddr, uintptr_t *cobankers, size_t offset, size_t len, uintptr_t *coresidents) {
	uint32_t tmp1, tmp2;
	uint64_t diffSamples[SAMPLE_SIZE];
	uint64_t start, stop;

	if(!oaddr) return 0;

//	const uintptr_t taddr[2] = { oaddr, oaddr+64 };
	for (int j = 0; j < SAMPLE_SIZE; j++) {
		rdtscp_begin(start, tmp1, tmp2);
		hammer_single(oaddr, MACCESS_ITERATIONS);
		//hammer_double(taddr, MACCESS_ITERATIONS);
		rdtscp_end(stop, tmp1, tmp2);
		diffSamples[j] = stop - start;
	}
	qsort(diffSamples, SAMPLE_SIZE, sizeof(uint64_t), u64cmp);
	uint64_t hit_time=diffSamples[1];

	//run the actual profiling
	uint64_t threshHold = (uint64_t)(hit_time*1.3);
	size_t found = 0;

	for (size_t i=offset; i<len; i++) {

		if(!cobankers[i]) continue;

		const uintptr_t taddr[2] = { oaddr, cobankers[i] };
		for (int j = 0; j < SAMPLE_SIZE; j++) {
			rdtscp_begin(start, tmp1, tmp2);
			hammer_double(taddr, MACCESS_ITERATIONS);
			rdtscp_end(stop, tmp1, tmp2);
			diffSamples[j] = stop - start;
		}
		qsort(diffSamples, SAMPLE_SIZE, sizeof(uint64_t), u64cmp);

//		uint64_t mdiff = diffSamples[SAMPLE_SIZE / 2];
		uint64_t mdiff = diffSamples[1];
		if (mdiff < threshHold){
			found++;
			if(found<PAGES_PER_ROW){
				coresidents[found-1] = i;
			} else if (found>PAGES_PER_ROW+2){
				found=0;
				break;
			}
		}
	}
	return MIN(found, PAGES_PER_ROW-1);
}

#if DEBUG==1
static int page_row_cmp(const void *p1, const void *p2) {
	const struct DRAM_ROW *page1 = (const struct DRAM_ROW*) p1;
	const struct DRAM_ROW *page2 = (const struct DRAM_ROW*) p2;
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

#define FLUSH(rows, rowIndex, pageIndex) do {								\
	flush(rows[rowIndex].vaddr[pageIndex],rows[rowIndex].dplen[pageIndex]);	\
} while (0)

#define FILLNFLUSH(rows, rowIndex, v) do {					\
	for(size_t _i=0;_i<(rows)[rowIndex].size;_i++){			\
		uintptr_t _currentPage=rows[rowIndex].vaddr[_i];	\
		size_t _currentPageLen=rows[rowIndex].dplen[_i];	\
		memset((void*)_currentPage, v, _currentPageLen);	\
		flush(_currentPage, _currentPageLen);				\
	}														\
} while (0)

int main(int argc, char *argv[]) {
	uint32_t *array;
	uintptr_t origin;
	uintptr_t found[BUFSIZE / ROW_GRAN];
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
	n = sbdr(origin, array, BUFSIZE, found, ROW_GRAN);

	if((uint64_t)n*ROW_GRAN>(uint64_t)(2.0*l3cache_size)){//a lot of pages will not be inside the cache
		fprintf(stderr, "[!]Using eviction buffer\n");
		evictionBuffer=(uint64_t*)malloc(l3cache_size);//evict the whole l3 cache instead
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

	size_t rowIndex=0;
	size_t totalFound=0;
	size_t coresidents[PAGES_PER_ROW];
	struct DRAM_ROW rows[n];

#if DEBUG==1
	size_t mistaken_brothers=0;
#endif
	for(size_t i=0;i<n;i++){
		if(!found[i]) continue;

		rows[rowIndex].drid = rowIndex;
		rows[rowIndex].vaddr[0] = found[i];
		rows[rowIndex].dplen[0] = ROW_GRAN;
		size_t n2=sr(found[i], found, i+1, n, coresidents);

		rows[rowIndex].size=n2+1;

		for(size_t j=0;j<n2;j++){
			rows[rowIndex].vaddr[j+1]=found[coresidents[j]];
			rows[rowIndex].dplen[j+1]=ROW_GRAN;
			found[coresidents[j]]=0;
			totalFound++;
		}

#if DEBUG==1
		rows[rowIndex].paddr = getPhysAddress(found[i]);
		rows[rowIndex].row = getRow(rows[rowIndex].paddr);
		rows[rowIndex].bank = getBank(rows[rowIndex].paddr);
		size_t mistaken_brothers=0;
		for(size_t j=0;j<rows[rowIndex].size;j++){
			if(rows[rowIndex].vaddr[j]!=rows[rowIndex].row)
				mistaken_brothers++;
		}
#endif

		rowIndex++;
	}

	fprintf(stderr, "Old Row Number=%zu, New=%zu\n", n, rowIndex);
	n=rowIndex; //update the new length

#if DEBUG==1
	{
		fprintf(stderr, "Mistaken brothers=%zu\n", mistaken_brothers);
		uint64_t originBank = getBank(getPhysAddress(origin));
		size_t err = 0;
		for (size_t i = 0; i < n; i++) {
			if (rows[i].bank != originBank)
				err++;
			fprintf(stderr, "Bank=%lu Row=%lu addr=0x%012lx phys=0x%012lx\n",
					rows[i].bank, rows[i].row, rows[i].vaddr[0], rows[i].paddr);
		}

		if (err > 0) {
			fprintf(stderr, "Found %zu possible misplacements in the bank\n", err);
		} else {
			fprintf(stderr, "Map seems to be accurate\n");
		}

		sleep(3);
		qsort(rows, n, sizeof(struct DRAM_ROW), page_row_cmp);
	}
#endif
	if (runTime > 0) {
		signal(SIGALRM, stop_signal);
		alarm(runTime);
	}

	signal(SIGINT, stop_signal);

	for (size_t i = 0; i < n; i++) {
		printf("[%zu]Testing %012lx\n", i, rows[i].vaddr[0]);

		if(i%10==0) fflush(NULL);

#if DEBUG==1
		{
			//check if the row is already tested
			if (i > 0 && rows[i - 1].row == rows[i].row)
				continue;

			//try to find triplets for double sided rowhammer
			size_t m=i+1;
			while(m<n && rows[m].row==rows[i].row && m++);
			if(m==n || rows[m].row!=rows[i].row+1) continue;
			size_t j=m++;
			while(m<n && rows[m].row==rows[j].row && m++);
			if(m==n || rows[m].row!=rows[i].row+2) continue;

			printf("Indexes (%zu=%lu, %zu=%lu, %zu=%lu)\n",
					i, rows[i].row,
					m, rows[m].row,
					j, rows[j].row);

			printf("........................\n");
		}
#endif

		//prepare the first target row for hammering
		FILLNFLUSH(rows, i, 0);
		uintptr_t dside = rows[i].vaddr[0];
		for (int j = i + 1; j < n; j++) {
			//prepare the other target row for hammering
			FILLNFLUSH(rows, j, 0);
			uintptr_t uside = rows[j].vaddr[0];

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
					for(size_t j=0; j < rows[i].size; j++)
						FLUSH(rows, i, j);
			}

			flip_check_label:
			for (size_t currentRow = 0; currentRow < n; currentRow++) {

				if (currentRow == i || currentRow == j)
					continue;

				for(size_t currentPage=0;currentPage<rows[currentRow].size;currentPage++){

					int gotBitFlips=0;

					for (size_t offset = 0; offset < rows[currentRow].dplen[currentPage]; offset++) {
						uint8_t got = *(volatile uint8_t*) (rows[currentRow].vaddr[currentPage] + offset);

						if (got != 0xff) {
							if(!externalRun){
								printf("+++++[%lu]+++++++[%lu]++++++++\n", rows[i].drid, rows[j].drid);
								hammer_double(taddr, STRESS_ITERATIONS);
								sleep(2);
								hammer_double(taddr, STRESS_ITERATIONS);
								sleep(2);

								externalRun++;
								goto flip_check_label;
							}

							if(!gotBitFlips) {
								printf("Victim Row ID=%lu\n", rows[currentRow].drid);
								gotBitFlips++;
							}

	#if DEBUG==1
							printf("(^0x%012lx=0x%012lx,^0x%012lx=0x%012lx) Found [0x%012lx=0x%012lx] bitflip [%zu]=0x%02x (^%lu,%lu,^%lu)\n",
									dside, rows[i].paddr,
									uside, rows[j].paddr,
									rows[currentRow].vaddr[currentPage], getPhysAddress(rows[currentRow].vaddr[currentPage]),
									offset,
									got & 0xff,
									rows[i].row, rows[currentRow].row, rows[j].row);
	#else
							printf("(0x%012lx,0x%012lx) Found [0x%012lx] bitflip [%zu]=0x%02x\n",
									dside, uside,
									rows[currentRow].vaddr[currentPage],
									offset,
									got&0xff);
	#endif
							*(volatile uint8_t*) (rows[currentRow].vaddr[currentPage] + offset) = 0xff;
						}
					}
					if(gotBitFlips) {
						FLUSH(rows, currentRow, currentPage);
						printf("---------------------\n");
					}
				}
			}
			FILLNFLUSH(rows, j, 0xff);
		}
		FILLNFLUSH(rows, i, 0xff);
	}
	return 0;
}
