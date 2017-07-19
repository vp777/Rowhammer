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

#ifndef HPRH_H_
#define HPRH_H_

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
	size_t bufSize;
	struct DramParams dramParams;
	char tfill;
	char vfill;
};

#endif /* HPRH_H_ */
