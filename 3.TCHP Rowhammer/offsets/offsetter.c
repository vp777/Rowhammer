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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <asm/errno.h>
#include <time.h>
#include <string.h>
#include <time.h>



#define BUFSIZ_DEFAULT (1<<23)
#define CHANNELS_DEFAULT 1
#define DIMMS_DEFAULT 1
#define RANKS_DEFAULT 2
#define RANK_MIRRORING_DEFAULT 0
#define MAP_GRAN_DEFAULT (1<<13)
#define VFILL_DEFAULT 0xff
#define TFILL_DEFAULT 0x00

#define PAGE_SIZE 0x1000
#define ROW_LEN (1<<13)
#define HUGE_PAGE_ROWS (HUGE_PAGE_SIZE/ROW_LEN)
#define CACHE_LINE_SIZE 64

#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

#define MASK(m) ((1ULL<<((m)%(sizeof(1ULL)*8)))-1)
#define GENMASK(l,r) (MASK((l)+1)^MASK(r))
#define BIT(n, x) (((n)>>(x))&1)
#define BITS(n, l, r) (((n)>>(r))&MASK((l)-(r)+1))
#define POP_BIT(n, x) ((((n)>>1)&~MASK(x))|((n)&MASK(x)))

struct DRAMAddr {
	uint8_t chan;
	uint8_t dimm;
	uint8_t rank;
	uint8_t bank;
	uint16_t row;
	uint16_t col;
};

struct HugePageEntry {
	uintptr_t virtAddr;
	struct DRAMAddr dramAddr;
};

struct HugePage {
	struct HugePageEntry *hentry;
	size_t numberOfEntries;
};

struct DramParams {
	uint8_t channels;
	uint8_t dimms;
	uint8_t ranks;
	uint8_t rank_mirroring;
	uint16_t map_gran;
};

struct HammerOptions {
	FILE *out;
	size_t bufSize;
	struct DramParams dramParams;
	char tfill;
	char vfill;
};

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

struct DRAMAddr sandyMap(uintptr_t vaddr, struct DramParams *dramParams) {
	struct DRAMAddr dramAddr = { 0 };

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
		dramAddr.rank = BIT(vaddr, 16) ^ BIT(vaddr, 20);
		vaddr = POP_BIT(vaddr, 16);
	} else {
		dramAddr.rank = 0;
	}
	dramAddr.bank = BITS(vaddr, 15, 13) ^ BITS(vaddr, 18, 16);
	dramAddr.row = BITS(vaddr, 31, 16);

	if (dramParams->rank_mirroring && dramAddr.rank) {
		dramAddr = ddr3_rank_mirror(dramAddr);
	}

	dramAddr.row ^= BIT(vaddr, 19) ? 6 : 0;

	return dramAddr;
}

void setupHPEntries(struct HugePage *hp, uintptr_t baseAddr, struct DramParams *dramParams) {
	for (size_t i = 0; i < hp->numberOfEntries; i++) {
		hp->hentry[i].virtAddr = baseAddr + i * dramParams->map_gran;
		hp->hentry[i].dramAddr = sandyMap(baseAddr + i * dramParams->map_gran, dramParams);
	}
}

#define HP_ENTRY_TO_INT(entry) (((uint64_t)(entry).dramAddr.col)|\
							((uint64_t)(entry).dramAddr.row<<15)|\
							((uint64_t)(entry).dramAddr.bank<<40)|\
							((uint64_t)(entry).dramAddr.rank<<45)|\
							((uint64_t)(entry).dramAddr.dimm<<50)|\
							((uint64_t)(entry).dramAddr.chan<<55))

int compare_hp_entries(const void *p, const void *q) {
	uint64_t x = HP_ENTRY_TO_INT(*(const struct HugePageEntry * )p);
	uint64_t y = HP_ENTRY_TO_INT(*(const struct HugePageEntry * )q);

	if (x < y)
		return -1;
	else if (x > y)
		return 1;

	return 0;
}

int compare_hp_entries_virt(const void *p, const void *q) {
	uintptr_t x = (*(const struct HugePageEntry * )p).virtAddr;
	uintptr_t y = (*(const struct HugePageEntry * )q).virtAddr;

	if (x < y)
		return -1;
	else if (x > y)
		return 1;

	return 0;
}

#undef HP_ENTRY_TO_INT

void setupMappings(struct HugePage **hps, uintptr_t startAddr, size_t len, struct DramParams *dramParams) {
	*hps = malloc(sizeof(**hps));
	(*hps)->numberOfEntries = len / dramParams->map_gran;
	(*hps)->hentry = malloc((*hps)->numberOfEntries * sizeof(struct HugePageEntry));
	setupHPEntries(*hps, startAddr, dramParams);
	qsort((*hps)->hentry, (*hps)->numberOfEntries, sizeof(struct HugePageEntry), compare_hp_entries);
}

enum access_type {
	ROW_BUFFER_HIT,
	ROW_BUFFER_MISS
};

int DRAMAcmp(struct DRAMAddr da1, struct DRAMAddr da2, enum access_type at){
	struct DRAMAddr tempda1=da1;
	struct DRAMAddr tempda2=da2;

	tempda1.col=0;
	tempda2.col=0;

	if(at==ROW_BUFFER_MISS){
		tempda1.row=0;
		tempda2.row=0;
	}

	return memcmp(&tempda1, &tempda2, sizeof(struct DRAMAddr))==0 &&
			(at==ROW_BUFFER_MISS?da1.row!=da2.row:1);
}

void printDRAMAddr(struct DRAMAddr da){
	printf("Channel=%" PRIu8 ", DIMM=%"PRIu8", RANK=%"PRIu8", BANK=%"PRIu8", ROW=%"PRIu16", COLUMN=%"PRIu16"\n",
			da.chan,
			da.dimm,
			da.rank,
			da.bank,
			da.row,
			da.col);
}

int main(int argc, char *argv[]) {
	struct HugePage *hp;

	struct HammerOptions ho;
	ho.bufSize = BUFSIZ_DEFAULT;
	ho.out = stdout;
	ho.dramParams = (struct DramParams ) { .channels = CHANNELS_DEFAULT, .ranks = RANKS_DEFAULT, .dimms = DIMMS_DEFAULT, .rank_mirroring = RANK_MIRRORING_DEFAULT,
					.map_gran = MAP_GRAN_DEFAULT };

	ho.dramParams.map_gran = (ho.dramParams.channels == 2 || ho.dramParams.rank_mirroring == 1) ? (1 << 6) : (1 << 13);

	setupMappings(&hp, 0, BUFSIZ_DEFAULT, &ho.dramParams);

	size_t sbdrLen=0,sbdrStart=-1;
	struct HugePageEntry firstEntry = hp->hentry[0];

	printDRAMAddr(firstEntry.dramAddr);

	for(size_t i=1;i<hp->numberOfEntries;i++){
		struct HugePageEntry centry = hp->hentry[i];
//		printf("[%zu] [%012lx] ", i, centry.virtAddr);
//		printDRAMAddr(centry.dramAddr);
		if(DRAMAcmp(firstEntry.dramAddr, centry.dramAddr, ROW_BUFFER_MISS)){
			sbdrLen++;
			if(sbdrStart==-1) sbdrStart=i;
		} else if (sbdrStart!=-1) break;
	}

	for(size_t i=sbdrStart;i<sbdrStart+sbdrLen;i++){
		int64_t diff=hp->hentry[i].virtAddr-hp->hentry[i-1].virtAddr;
		printf("[%zu] 0x%lx (%ld)\n", i, diff, diff);
	}

	return 0;
}
