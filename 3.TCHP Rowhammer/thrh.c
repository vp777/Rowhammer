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
#include "hprh.h"

uint64_t BUFSIZE = (1 << 29);
uint64_t MACCESS_ITERATIONS = 5000;
uint64_t SAMPLE_SIZE = 13;
uint64_t CALIBRATION_RUNS = 64;
uint64_t TEST_ITERATIONS = 550000;
uint64_t STRESS_ITERATIONS = 1700000;
float THRESHOLD_MULT = 1.3;

#define PAGE_SIZE 0x1000
#define ROW_GRAN 0x1000 //optional: on sandy bridge, the whole page resides in a single row.
#define ROW_LEN (1<<13)
#define PAGES_PER_ROW (ROW_LEN/ROW_GRAN)

#define BUFSIZ_DEFAULT (1<<23)
#define CHANNELS_DEFAULT 1
#define DIMMS_DEFAULT 1
#define RANKS_DEFAULT 2
#define RANK_MIRRORING_DEFAULT 0
#define VFILL_DEFAULT 0xff //0x6d
#define TFILL_DEFAULT 0x00//0x92

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

static int u64cmp(const void *p1, const void *p2) {
	const uint64_t v1 = *(uint64_t const *) p1;
	const uint64_t v2 = *(uint64_t const *) p2;
	return v1 > v2 ? 1 : (v1 == v2 ? 0 : -1);
}

uint64_t access_time(const uintptr_t *addrs) {
	uint32_t tmp1, tmp2;
	uint64_t start, stop;

	rdtscp_begin(start, tmp1, tmp2)
	hammer_double(addrs, MACCESS_ITERATIONS);
	rdtscp_end(stop, tmp1, tmp2)
	return stop - start;
}

uint64_t sample_pair(uintptr_t addr1, uintptr_t addr2, size_t v) {
	uint64_t diffSamples[SAMPLE_SIZE];
	const uintptr_t pair[] = { addr1, addr2 };

	for (int j = 0; j < SAMPLE_SIZE; j++) {
		diffSamples[j] = access_time(pair);
	}
	qsort(diffSamples, SAMPLE_SIZE, sizeof(uint64_t), u64cmp);
	return diffSamples[v];
}

//Sandy Bridge specific
uintptr_t findContiguousRegion(void *mem, size_t len) {
	uint64_t threshHold = (uint64_t) (sample_pair((uintptr_t) mem, (uintptr_t) mem + 128, 0) * THRESHOLD_MULT);

	fprintf(stderr, "Threshold=%lu\n", threshHold);
	for(uintptr_t buf=(uintptr_t)mem;buf<(uintptr_t)mem+len;buf+=7*PAGE_SIZE){//take into account the buf+7*0x22000+.... in bound checking
		if(		0
				|| sample_pair(buf, buf+7*0x22000+0xee000, 0)<threshHold //with this we span about 2mb, if no results try putting it in comments
				|| sample_pair(buf, buf+7*0x22000, 0)<threshHold
				|| sample_pair(buf, buf+6*0x22000, 0)<threshHold
				|| sample_pair(buf, buf+5*0x22000, 0)<threshHold
				|| sample_pair(buf, buf+4*0x22000, 0)<threshHold
				|| sample_pair(buf, buf+1*0x22000, 0)<threshHold
				|| sample_pair(buf, buf+2*0x22000, 0)<threshHold
				){
			continue;
		}

		if(sample_pair(buf, buf+0x23000, 0) < threshHold){
			buf-=0x1000;
			fprintf(stderr, "Aligned\n");
		}

		//try to find the beginning of the region
		//uintptr_t current=buf;
		//while(sample_pair(current, current+0x23000, 0)>threshHold /*&& sample_pair(current, current-0x1000, 0)<threshHold*/){
		//	current-=0x2000;
		//}
		//buf=current;
		
		return buf;
	}
	return 0;
}

void stop_signal(int signal) {
	fflush(NULL);
	exit(0);
}

int main(int argc, char *argv[]) {
	void *buf, *targetbuf;
	uintptr_t target;
	uint64_t runTime=0;

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

	if(runTime>0){
		signal(SIGALRM, stop_signal);
		alarm(runTime);
	}

	buf = mmap(NULL, BUFSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	memset((void*) buf, 0xff, BUFSIZE);
	target = findContiguousRegion(buf, BUFSIZE);
	if(!target) exit(0);

	struct HugePage *hps;
	struct HammerOptions ho;

	ho.bufSize = (1 << 21);
	ho.dramParams = (struct DramParams ) { .channels = CHANNELS_DEFAULT, .ranks = RANKS_DEFAULT, .dimms = DIMMS_DEFAULT, .rank_mirroring = RANK_MIRRORING_DEFAULT, };
	ho.tfill = TFILL_DEFAULT;
	ho.vfill = VFILL_DEFAULT;

	ho.dramParams.map_gran = (ho.dramParams.channels == 2 || ho.dramParams.rank_mirroring == 1) ? (1 << 6) : (1 << 13);

	targetbuf = (void*) target;

	memset(targetbuf, ho.tfill, ho.bufSize);
	flush((uintptr_t) targetbuf, ho.bufSize);

	size_t totalHPages = setupHugePages(&hps, 0, ho.bufSize, &ho.dramParams);
	for (size_t i = 0; i < hps->numberOfEntries; i++)
		hps->hentry[i].virtAddr += (uintptr_t)targetbuf;
	
	runTest(hps, totalHPages, &ho);
}
