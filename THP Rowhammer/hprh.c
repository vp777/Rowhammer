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

#include <stdio.h>
#include <sys/signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <asm/errno.h>
#include <time.h>
#include <string.h>
#include <sched.h>
#include "hprh.h"


#define PASSES 1 //Can be used to check the reliability of bitflips
#define WINDOW_RAD 0
#define ITERATIONS 1966080 //over a million will probably span over multiple refresh intervals

#define BUFSIZ_DEFAULT (1<<21)
#define CHANNELS_DEFAULT 1
#define DIMMS_DEFAULT 1
#define RANKS_DEFAULT 2
#define RANK_MIRRORING_DEFAULT 1
#define VFILL_DEFAULT 0xff //0x6d
#define TFILL_DEFAULT 0x00 //0x92

#define PAGE_SIZE 0x1000
#define ROW_LEN (1<<13)
#define HUGE_PAGE_SIZE (1<<21)
#define HUGE_PAGE_ROWS (HUGE_PAGE_SIZE/ROW_LEN)
#define CACHE_LINE_SIZE 64

uint16_t totalBitFlips = 0;

void hammer_double(const uintptr_t *addrs, unsigned int itercount) {
	volatile int *p = (volatile int *) (addrs[0]);
	volatile int *q = (volatile int *) (addrs[1]);
	while (itercount--) {
		*p;
		*q;
		asm volatile (
				"clflush (%0)\n\t"
				"clflush (%1)\n\t"
				:
				: "r" (p), "r" (q)
				: "memory"
		);
	}
}

uint32_t getCacheLineSize(){
	uint32_t info;
	asm volatile ("mov $0x80000006, %%eax\n\t"
				"CPUID\n\t"
				"mov %%ecx, %0\n\t"
				:"=r"(info)
				:
				:"%rax", "%rbx", "%rcx", "%rdx");
	return BITS(info, 7, 0);
}

void flush(uintptr_t vaddr, size_t len) {
	for (uintptr_t caddr = vaddr; caddr < vaddr + len; caddr += CACHE_LINE_SIZE) {
		asm volatile ("clflush (%0)\n\t": : "r" (caddr) : "memory");
	}
}

//source: https://github.com/vusec/hammertime/
struct DRAMAddr ddr3_rank_mirror(struct DRAMAddr addr) {
	struct DRAMAddr ret = addr;
	/* Switch address bits 3<->4 5<->6 7<->8 */
	ret.row &= 0xfe07;
	ret.row |= (BIT(addr.row, 7) << 8) | (BIT(addr.row, 8) << 7) | (BIT(addr.row, 5) << 6) | (BIT(addr.row, 6) << 5) | (BIT(addr.row, 3) << 4) | (BIT(addr.row, 4) << 3);
	ret.col &= 0xfe07;
	ret.col |= (BIT(addr.col, 7) << 8) | (BIT(addr.col, 8) << 7) | (BIT(addr.col, 5) << 6) | (BIT(addr.col, 6) << 5) | (BIT(addr.col, 3) << 4) | (BIT(addr.col, 4) << 3);
	/* Switch bank bits 0<->1 */
	ret.bank &= 0xfc;
	ret.bank |= (BIT(addr.bank, 0) << 1) | BIT(addr.bank, 1);

	return ret;
}

//source: https://github.com/vusec/hammertime/ (based on reverse engineered dram mapping)
struct DRAMAddr partialSandyMap(uintptr_t vaddr, struct DramParams *dramParams) {
	struct DRAMAddr dramAddr = { 0 };

	vaddr &= MASK(21); //we are parsing each page separately so not that important

	if (dramParams->channels == 2) {
		dramAddr.col = BITS(vaddr, 5, 3);
		dramAddr.chan = BIT(vaddr, 6);
		vaddr = POP_BIT(vaddr, 6);
		dramAddr.col = BITS(vaddr, 12, 6);
	} else {
		dramAddr.col = BITS(vaddr, 12, 3);
	}

	if (dramParams->dimms == 2) {
		dramAddr.dimm = BIT(vaddr, 16);
		vaddr = POP_BIT(vaddr, 16);
	}

	if (dramParams->ranks == 2) {
		dramAddr.rank = BIT(vaddr, 16);
		vaddr = POP_BIT(vaddr, 16);
	} else {
		dramAddr.rank = 0;
	}
	dramAddr.bank = BITS(vaddr, 15, 13) ^ BITS(vaddr, 18, 16);
	dramAddr.row = BITS(vaddr, 20, 16);

	if (dramParams->rank_mirroring && dramAddr.rank) {
		dramAddr = ddr3_rank_mirror(dramAddr);
	}

	dramAddr.row ^= BIT(vaddr, 19)?6:0; //on-dimm remapping

	return dramAddr;
}

void setupHPEntries(struct HugePage *hp, uintptr_t baseAddr, struct DramParams *dramParams) {
	for (size_t i = 0; i < hp->numberOfEntries; i++) {
		hp->hentry[i].virtAddr = baseAddr + i * dramParams->map_gran;
		hp->hentry[i].dramAddr = partialSandyMap(baseAddr + i * dramParams->map_gran, dramParams);
	}
}

#define HP_ENTRY_TO_INT(entry) (((uint64_t)(entry).dramAddr.col)|\
							((uint64_t)(entry).dramAddr.row<<15)|\
							((uint64_t)(entry).dramAddr.bank<<40)|\
							((uint64_t)(entry).dramAddr.chan<<45))|\
							((uint64_t)(entry).dramAddr.rank<<50)|\
							((uint64_t)(entry).dramAddr.dimm<<55)

int compare_hp_entries(const void *p, const void *q) {
	uint64_t x = HP_ENTRY_TO_INT(*(const struct HugePageEntry *)p);
	uint64_t y = HP_ENTRY_TO_INT(*(const struct HugePageEntry *)q);

	if (x < y)
		return -1;
	else if (x > y)
		return 1;

	return 0;
}

#undef HP_ENTRY_TO_INT

size_t setupHugePages(struct HugePage **hps, uintptr_t startAddr, size_t len, struct DramParams *dramParams) {
	size_t i = 0;
	*hps = malloc((len / HUGE_PAGE_SIZE) * sizeof(**hps));
	for (uintptr_t currentHPage = (uintptr_t) startAddr; currentHPage < startAddr + len; currentHPage += HUGE_PAGE_SIZE, i++) {
		(*hps + i)->numberOfEntries = MIN(startAddr + len - currentHPage, HUGE_PAGE_SIZE) / dramParams->map_gran;
		(*hps + i)->hentry = malloc((*hps + i)->numberOfEntries * sizeof(struct HugePageEntry));
		setupHPEntries(*hps + i, currentHPage, dramParams);
		qsort((*hps + i)->hentry, (*hps + i)->numberOfEntries, sizeof(struct HugePageEntry), compare_hp_entries);
	}
	return i;
}

//accurate-enough for the testing purposes
#define GET_ROW_BASE_ENTRY(rowNum, rowLen, memGran) ((rowNum) * (rowLen) / (memGran))
#define GET_ROW_BASE_ENTRY_SAFE(rowNum, rowLen, memGran, maxRow) ((MIN(MAX(rowNum, 0), maxRow)) * (rowLen) / (memGran))
#define FOR_WINDOW_ROWS(entry, victimRow, windowRad, entriesNum) for (size_t entry = MAX((int64_t)(victimRow)-(windowRad), 0); entry < MIN((victimRow)+(windowRad)+1, entriesNum); entry++)
#define FOR_ROW_ENTRIES(entry, rowNum, rowLen, mapGran, totalEntries) for (size_t entry = GET_ROW_BASE_ENTRY(rowNum,rowLen,mapGran); entry < MIN(GET_ROW_BASE_ENTRY((rowNum) + 1, rowLen, mapGran), totalEntries); entry++)

void runTest(struct HugePage *hps, size_t totalHPages, struct HammerOptions *hammerOptions) {
	struct DramParams dramParams = hammerOptions->dramParams;
	struct HugePage *currentHugePage;

	for (size_t ihp = 0; ihp < totalHPages; ihp++) {
		sleep(1);
		currentHugePage = &hps[ihp];
		for (size_t victimRow = 1; victimRow < currentHugePage->numberOfEntries*dramParams.map_gran/ROW_LEN-1; victimRow++) {

			size_t t1Index = GET_ROW_BASE_ENTRY(victimRow - 1, ROW_LEN, dramParams.map_gran);
			uintptr_t target1 = currentHugePage->hentry[t1Index].virtAddr;

			size_t t2Index = GET_ROW_BASE_ENTRY(victimRow + 1, ROW_LEN, dramParams.map_gran);
			uintptr_t target2 = currentHugePage->hentry[t2Index].virtAddr;

			uintptr_t hamaddr[2] = { target1, target2 };

			struct DRAMAddr t = currentHugePage->hentry[GET_ROW_BASE_ENTRY(victimRow - 1, ROW_LEN, dramParams.map_gran)].dramAddr;
			printf("Hammering %p and %p -> Channel=%u, Dimm=%u, Rank=%u, Bank=%u, relRow1=%u\n",
					(void*) hamaddr[0],
					(void*) hamaddr[1],
					t.chan, t.dimm, t.rank,t.bank, t.row);

			for (int pass = 0; pass < PASSES; pass++) {
				FOR_WINDOW_ROWS(rowNum, victimRow, WINDOW_RAD, HUGE_PAGE_ROWS)
				{
					if (rowNum == victimRow - 1 || rowNum == victimRow + 1)
						continue;
					FOR_ROW_ENTRIES(entryIndex, rowNum, ROW_LEN, dramParams.map_gran, currentHugePage->numberOfEntries)
					{
						memset((void*) currentHugePage->hentry[entryIndex].virtAddr, hammerOptions->vfill, dramParams.map_gran);
						flush(currentHugePage->hentry[entryIndex].virtAddr, dramParams.map_gran);
					}
				}

				FOR_ROW_ENTRIES(entryIndex, victimRow-1, ROW_LEN, dramParams.map_gran, currentHugePage->numberOfEntries)
				{
					memset((void*) currentHugePage->hentry[entryIndex].virtAddr, hammerOptions->tfill, dramParams.map_gran);
					flush(currentHugePage->hentry[entryIndex].virtAddr, dramParams.map_gran);
				}

				FOR_ROW_ENTRIES(entryIndex, victimRow+1, ROW_LEN, dramParams.map_gran, currentHugePage->numberOfEntries)
				{
					memset((void*) currentHugePage->hentry[entryIndex].virtAddr, hammerOptions->tfill, dramParams.map_gran);
					flush(currentHugePage->hentry[entryIndex].virtAddr, dramParams.map_gran);
				}

				hammer_double(hamaddr, ITERATIONS);

				FOR_WINDOW_ROWS(rowNum, victimRow, WINDOW_RAD, HUGE_PAGE_ROWS)
				{
					if (rowNum == victimRow - 1 || rowNum == victimRow + 1)
						continue;
					FOR_ROW_ENTRIES(entryIndex, rowNum, ROW_LEN, dramParams.map_gran, currentHugePage->numberOfEntries)
					{
						uintptr_t currentRowCAddr = currentHugePage->hentry[entryIndex].virtAddr;

						for (size_t i = 0; i < dramParams.map_gran; i++) {
							char expectedChar = hammerOptions->vfill;
							char got = *(volatile char*) (currentRowCAddr + i);
							if (got != expectedChar) {
								printf("[%d]Found bitflip addr[%d]=%p[%zu] 0x%02x->0x%02x\n",
										pass,
										(int) (victimRow - rowNum),
										(void*) currentRowCAddr,
										(entryIndex - GET_ROW_BASE_ENTRY(rowNum, ROW_LEN, dramParams.map_gran)) * dramParams.map_gran + i,
										hammerOptions->vfill & 0xff,
										got & 0xff);
								totalBitFlips++;
							}
						}
					}
				}
			}
		}
	}
	printf("Total bitflips=%u\n", totalBitFlips);
}

#undef GET_ROW_BASE_ENTRY
#undef GET_ROW_BASE_ENTRY_SAFE
#undef FOR_WINDOW_ROWS
#undef FOR_ROW_ENTRIES

void stop_signal(int signal){
	printf("Time is up\nTotal bitflips=%u\n", totalBitFlips);
	fflush(NULL);
	exit(0);
}

int main(int argc, char *argv[]) {
	void *buf;
	struct HugePage *hps;

	struct HammerOptions ho;
	ho.bufSize = BUFSIZ_DEFAULT;
	ho.dramParams = (struct DramParams ) {
		.channels = CHANNELS_DEFAULT,
		.ranks = RANKS_DEFAULT,
		.dimms = DIMMS_DEFAULT,
		.rank_mirroring = RANK_MIRRORING_DEFAULT,
	};
	ho.tfill = TFILL_DEFAULT;
	ho.vfill = VFILL_DEFAULT;

	uint64_t runTime=0;
	int opt;
	while ((opt = getopt(argc, argv, "s:c:d:r:mo:t:v:e:")) != -1) {
		switch (opt) {
			case 's': {
				char *end = NULL;
				ho.bufSize = strtoll(optarg, &end, 0) << (20);
				break;
			}
			case 'o':
				if(!freopen(optarg, "w", stdout)){
					perror("Cannot write to the given output file:");
				}
				break;
			case 'c': {
				char *end = NULL;
				ho.dramParams.channels = strtoll(optarg, &end, 0);
				break;
			}
			case 'd': {
				char *end = NULL;
				ho.dramParams.dimms = strtoll(optarg, &end, 0);
				break;
			}
			case 'r': {
				char *end = NULL;
				ho.dramParams.ranks = strtoll(optarg, &end, 0);
				break;
			}
			case 'm':
				ho.dramParams.rank_mirroring = 1;
				break;
			case 't': {
				char *end = NULL;
				ho.tfill = strtoll(optarg, &end, 0);
				break;
			}
			case 'v': {
				char *end = NULL;
				ho.vfill = strtoll(optarg, &end, 0);
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
	ho.dramParams.map_gran = (ho.dramParams.channels == 2 || ho.dramParams.rank_mirroring == 1) ? (1 << 6) : (1 << 13);

	//could also have used posix_memalign
	buf = mmap(NULL, ho.bufSize+HUGE_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (buf == MAP_FAILED) {
		perror("Buffer allocation failed");
		return -1;
	}
	buf = (void *)(((uintptr_t) buf + MASK(21)) & ~MASK(21));

	madvise(buf, ho.bufSize, MADV_HUGEPAGE);
	memset(buf, ho.tfill, ho.bufSize);
	flush((uintptr_t)buf, ho.bufSize);

	if(runTime>0){
		signal(SIGALRM, stop_signal);
		alarm(runTime);
	}

	size_t totalHPages = setupHugePages(&hps, (uintptr_t) buf, ho.bufSize, &ho.dramParams);
	runTest(hps, totalHPages, &ho);

	return 0;
}
