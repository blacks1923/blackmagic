/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2022 1BitSquared <info@1bitsquared.com>
 * Written by Rafael Silva <perigoso@riseup.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Support for Renesas RA family of microcontrollers (Arm Core) */

#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"
#include "adiv5.h"

#define RENESAS_PARTID_RA6M2 0x0150
#define RENESAS_PARTID_RA4M3 0x0310

/*
 * Part numbering scheme
 *
 *  R7   F   A   xx   x   x   x   x   x   xx
 * \__/ \_/ \_/ \__/ \_/ \_/ \_/ \_/ \_/ \__/
 *  |    |   |   |    |   |   |   |   |   |
 *  |    |   |   |    |   |   |   |   |   \_ Package type
 *  |    |   |   |    |   |   |   |   \_____ Quality Grade
 *  |    |   |   |    |   |   |   \_________ Operating temperature
 *  |    |   |   |    |   |   \_____________ Code flash memory size
 *  |    |   |   |    |   \_________________ Feature set
 *  |    |   |   |    \_____________________ Group number
 *  |    |   |   \__________________________ Series name
 *  |    |   \______________________________ family (A: RA)
 *  |    \__________________________________ Flash memory
 *  \_______________________________________ Renesas microcontroller (always 'R7')
 *
 * Renesas Flash MCUs have an internal 16 byte read only register that stores
 * the part number, the code is stored ascii encoded, starting from the lowest memory address
 * except for pnrs stored in 'FIXED_PNR1', where the code is stored in reverse order (but the last 3 bytes are still 0x20 aka ' ')
 */

/* family + series + group no */
#define PNR_FAMILY_INDEX                   3
#define PNR_SERIES(pnr3, pnr4, pnr5, pnr6) (((pnr3) << 24) | ((pnr4) << 16) | ((pnr5) << 8) | (pnr6))
typedef enum {
	PNR_SERIES_RA2L1 = PNR_SERIES('A', '2', 'L', '1'),
	PNR_SERIES_RA2E1 = PNR_SERIES('A', '2', 'E', '1'),
	PNR_SERIES_RA2E2 = PNR_SERIES('A', '2', 'E', '2'),
	PNR_SERIES_RA2A1 = PNR_SERIES('A', '2', 'A', '1'),
	PNR_SERIES_RA4M1 = PNR_SERIES('A', '4', 'M', '1'),
	PNR_SERIES_RA4M2 = PNR_SERIES('A', '4', 'M', '2'),
	PNR_SERIES_RA4M3 = PNR_SERIES('A', '4', 'M', '3'),
	PNR_SERIES_RA4E1 = PNR_SERIES('A', '4', 'E', '1'),
	PNR_SERIES_RA4W1 = PNR_SERIES('A', '4', 'W', '1'),
	PNR_SERIES_RA6M1 = PNR_SERIES('A', '6', 'M', '1'),
	PNR_SERIES_RA6M2 = PNR_SERIES('A', '6', 'M', '2'),
	PNR_SERIES_RA6M3 = PNR_SERIES('A', '6', 'M', '3'),
	PNR_SERIES_RA6M4 = PNR_SERIES('A', '6', 'M', '4'),
	PNR_SERIES_RA6M5 = PNR_SERIES('A', '6', 'M', '5'),
	PNR_SERIES_RA6E1 = PNR_SERIES('A', '6', 'E', '1'),
	PNR_SERIES_RA6T1 = PNR_SERIES('A', '6', 'T', '1'),
	PNR_SERIES_RA6T2 = PNR_SERIES('A', '6', 'T', '2'),
} pnr_series_t;

/* Code flash memory size */
#define PNR_MEMSIZE_INDEX 8
typedef enum {
	PNR_MEMSIZE_16KB = '3',
	PNR_MEMSIZE_32KB = '5',
	PNR_MEMSIZE_64KB = '7',
	PNR_MEMSIZE_128KB = '9',
	PNR_MEMSIZE_256KB = 'B',
	PNR_MEMSIZE_384KB = 'C',
	PNR_MEMSIZE_512KB = 'D',
	PNR_MEMSIZE_768KB = 'E',
	PNR_MEMSIZE_1MB = 'F',
	PNR_MEMSIZE_1_5MB = 'G',
	PNR_MEMSIZE_2MB = 'H',
} pnr_memsize_t;

/* For future reference, if we want to add an info command
 *
 * Package type
 * FP: LQFP 100 pins 0.5 mm pitch
 * FN: LQFP 80 pins 0.5 mm pitch
 * FM: LQFP 64 pins 0.5 mm pitch
 * FL: LQFP 48 pins 0.5 mm pitch
 * NE: HWQFN 48 pins 0.5 mm pitch
 * FK: LQFP 64 pins 0.8 mm pitch
 * BU: BGA 64 pins 0.4 mm pitch
 * LM: LGA 36 pins 0.5 mm pitch
 * FJ: LQFP 32 pins 0.8 mm pitch
 * NH: HWQFN 32 pins 0.5 mm pitch
 * BV: WLCSP 25 pins 0.4 mm pitch
 * BT: BGA 36 pins
 * NK: HWQFN 24 pins 0.5 mm pitch
 * NJ: HWQFN 20 pins 0.5 mm pitch
 * BY: WLCSP 16 pins 0.4 mm pitch
 * NF: QFN 40 pins
 * LJ: LGA 100 pins
 * NB: QFN 64 pins
 * FB: LQFP 144 pins
 * NG: QFN 56 pins
 * LK: LGA 145 pins
 * BG: BGA 176 pins
 * FC: LQFP 176 pins
 *
 * Quality ID
 * C: Industrial applications
 * D: Consumer applications
 *
 * Operating temperature
 * 2: -40°C to +85°C
 * 3: -40°C to +105°C
 * 4: -40°C to +125°C
 */

/* PNR/UID location by series
 * newer series have a 'Flash Root Table'
 * older series have a fixed location in the flash memory
 *
 * ra2l1 - Fixed location 1
 * ra2e1 - Fixed location 1
 * ra2e2 - Fixed location 1
 * ra2a1 - *undocummented
 * ra4m1 - *undocummented
 * ra4m2 - *undocummented
 * ra4m3 - Fixed location 2 *undocummented
 * ra4e1 - Fixed location 2
 * ra4w1 - *undocummented
 * ra6m1 - Flash Root Table
 * ra6m2 - Flash Root Table
 * ra6m3 - Flash Root Table
 * ra6m4 - Fixed location 2
 * ra6m5 - Fixed location 2
 * ra6e1 - Fixed location 2
 * ra6t1 - Flash Root Table
 * ra6t2 - Fixed location 2
 */
#define RENESAS_FIXED1_UID    0x01001C00UL /* Unique ID Register */
#define RENESAS_FIXED1_PNR    0x01001C10UL /* Part Numbering Register */
#define RENESAS_FIXED1_MCUVER 0x01001C20UL /* MCU Version Register */

#define RENESAS_FIXED2_UID    0x01008190UL /* Unique ID Register */
#define RENESAS_FIXED2_PNR    0x010080F0UL /* Part Numbering Register */
#define RENESAS_FIXED2_MCUVER 0x010081B0UL /* MCU Version Register */

/* The FMIFRT is a read-only register that stores the Flash Root Table address */
#define RENESAS_FMIFRT             0x407FB19CUL
#define RENESAS_FMIFRT_UID(frt)    (frt + 0x14UL) /* UID Register offset from Flash Root Table */
#define RENESAS_FMIFRT_PNR(frt)    (frt + 0x24UL) /* PNR Register offset from Flash Root Table */
#define RENESAS_FMIFRT_MCUVER(frt) (frt + 0x44UL) /* MCUVER Register offset from Flash Root Table */

/* System Control OCD Control */
#define RENESAS_SYOCDCR 0x4001E40EUL /* System Control OCD Control Register */
#define SYOCDCR_DBGEN   (1 << 7)     /* Debug Enable */


static bool renesas_uid(target *t, int argc, const char **argv);

const struct command_s renesas_cmd_list[] = {
	{"uid", renesas_uid, "Prints unique number"},
	{NULL, NULL, NULL},
};

static uint32_t renesas_fmifrt_read(target *t)
{
	return target_mem_read32(t, RENESAS_FMIFRT);
}

static void renesas_uid_read(target *t, const uint32_t base, uint8_t *uid)
{
	uint32_t uidr[4];
	for (size_t i = 0U; i < 4U; i++)
		uidr[i] = target_mem_read32(t, base + i * 4U);

	for (size_t i = 0U; i < 16U; i++)
		uid[i] = uidr[i / 4U] >> (i & 3U) * 8U; /* & 3U == % 4U */
}

static bool renesas_pnr_read(target *t, const uint32_t base, uint8_t *pnr)
{
	uint32_t pnrr[4];
	for (size_t i = 0U; i < 4U; i++)
		pnrr[i] = target_mem_read32(t, base + i * 4U);

	if (base == RENESAS_FIXED1_PNR) {
		/* Renesas... look what you made me do...  */
		/* reverse order, see 'Part numbering scheme' note for context */
		for (size_t i = 0U; i < 13U; i++)
			pnr[i] = pnrr[3U - (i + 3U) / 4U] >> (24U - ((i + 3U) & 3U) * 8U); /* & 3U == % 4U */
		memset(pnr + 13, 0x20, 3);
	} else {
		for (size_t i = 0; i < 16; i++)
			pnr[i] = pnrr[i / 4U] >> (i & 3U) * 8U; /* & 3U == % 4U */
	}

	/* all Renesas mcus start with 'R7', sanity check */
	return pnr[0] == 'R' && pnr[1] == '7';
}

static pnr_series_t renesas_series(const uint8_t *pnr)
{
	uint32_t series = 0;
	for (size_t i = 0; i < 4; i++)
		series = (series << 8) | pnr[PNR_FAMILY_INDEX + i];

	return (pnr_series_t)series;
}

static uint32_t renesas_flash_size(const uint8_t *pnr)
{
	switch (pnr[PNR_MEMSIZE_INDEX]) {
	case PNR_MEMSIZE_16KB:
		return UINT32_C(16 * 1024);
	case PNR_MEMSIZE_32KB:
		return UINT32_C(32 * 1024);
	case PNR_MEMSIZE_64KB:
		return UINT32_C(64 * 1024);
	case PNR_MEMSIZE_128KB:
		return UINT32_C(128 * 1024);
	case PNR_MEMSIZE_256KB:
		return UINT32_C(256 * 1024);
	case PNR_MEMSIZE_384KB:
		return UINT32_C(384 * 1024);
	case PNR_MEMSIZE_512KB:
		return UINT32_C(512 * 1024);
	case PNR_MEMSIZE_768KB:
		return UINT32_C(768 * 1024);
	case PNR_MEMSIZE_1MB:
		return UINT32_C(1024 * 1024);
	case PNR_MEMSIZE_1_5MB:
		return UINT32_C(1536 * 1024);
	case PNR_MEMSIZE_2MB:
		return UINT32_C(2048 * 1024);
	default:
		return 0;
	}
}

static int renesas_flash_erase(target_flash_s *f, target_addr_t addr, size_t len)
{
	return 0;
}

static int renesas_flash_write(target_flash_s *f, target_addr_t dest, const void *src, size_t len) {
	return 0;
}

static void renesas_add_flash(target *t, target_addr_t addr, size_t length, size_t page_size)
{
	target_flash_s *f = calloc(1, sizeof(*f));
	if (!f) /* calloc failed: heap exhaustion */
		return;

	f->start = addr;
	f->length = length;
	f->erased = 0xffU;
	f->blocksize = page_size;
	f->erase = renesas_flash_erase;
	f->write = renesas_flash_write;
	target_add_flash(t, f);
}

static void renesas_add_flash(target *t, target_addr_t addr, size_t length)
{
	renesas_priv_s *priv_storage = (renesas_priv_s *)t->target_storage;
	if (!priv_storage)
		return;

	/* Renesas RA MCUs can have one of two kinds of flash memory, MF3/4 and RV40
	 * Flash type by series:
	 * ra2l1 - MF4
	 * ra2e1 - MF4
	 * ra2e2 - MF4
	 * ra2a1 - MF3
	 * ra4m1 - MF3
	 * ra4m2 - RV40
	 * ra4m3 - RV40
	 * ra4e1 - RV40
	 * ra4w1 - MF3
	 * ra6m1 - RV40
	 * ra6m2 - RV40
	 * ra6m3 - RV40
	 * ra6m4 - RV40
	 * ra6m5 - RV40
	 * ra6e1 - RV40
	 * ra6t1 - RV40
	 * ra6t2 - RV40
	 */

	switch (priv_storage->series) {
	case PNR_SERIES_RA2L1:
	case PNR_SERIES_RA2E1:
	case PNR_SERIES_RA2E2:
	case PNR_SERIES_RA2A1:
	case PNR_SERIES_RA4M1:
	case PNR_SERIES_RA4W1:
		return;

	case PNR_SERIES_RA4M2:
	case PNR_SERIES_RA4M3:
	case PNR_SERIES_RA4E1:
	case PNR_SERIES_RA6M1:
	case PNR_SERIES_RA6M2:
	case PNR_SERIES_RA6M3:
	case PNR_SERIES_RA6M4:
	case PNR_SERIES_RA6E1:
	case PNR_SERIES_RA6M5:
	case PNR_SERIES_RA6T1:
	case PNR_SERIES_RA6T2:
		t->enter_flash_mode = renesas_enter_flash_mode;
		return renesas_add_rv40_flash(t, addr, length);

	default:
		return;
	}
}

bool renesas_probe(target *t)
{
	uint8_t pnr[16]; /* 16-byte PNR */
	uint32_t flash_root_table = 0;

	/* Enable debug */
	/* a read back doesn't seem to show the change, tried 32-bit write too */
	/* See "DBGEN": Section 2.13.1 of the RA6M4 manual R01UH0890EJ0100. */
	target_mem_write8(t, SYSC_SYOCDCR, SYOCDCR_DBGEN);

	/* Read the PNR */
	switch (t->part_id) {
		// case :
		/* mcus with PNR located at 0x01001C10
		 * ra2l1 (part_id wanted)
		 * ra2e1 (part_id wanted)
		 * ra2e2 (part_id wanted)
		 */
		// if (!renesas_pnr_read(t, RENESAS_FIXED1_PNR, pnr))
		//	return false;
		// break;

	case RENESAS_PARTID_RA4M3:
		/* mcus with PNR located at 0x010080F0
		 * ra4e1 (part_id wanted)
		 * ra6m4 (part_id wanted)
		 * ra6m5 (part_id wanted)
		 * ra6e1 (part_id wanted)
		 * ra6t2 (part_id wanted)
		 */
		if (!renesas_pnr_read(t, RENESAS_FIXED2_PNR, pnr))
			return false;
		break;

	case RENESAS_PARTID_RA6M2:
		/* mcus with Flash Root Table
		 * ra6m1 (part_id wanted)
		 * ra6m3 (part_id wanted)
		 * ra6t1 (part_id wanted)
		 */
		flash_root_table = renesas_fmifrt_read(t);
		if (!renesas_pnr_read(t, RENESAS_FMIFRT_PNR(flash_root_table), pnr))
			return false;

		break;

	default:
		/*
		 * unknown part_id, we know this AP is from renesas, so Let's try brute forcing
		 * unfortunately, this is will lead to illegal memory accesses,
		 * but experimentally there doesn't seem to be an issue with these in particular
		 *
		 * try the fixed address RENESAS_FIXED2_PNR first, as it should lead to less illegal/erroneous
		 * memory accesses in case of failure, and is the most common case
		 */
		/*
		 * ra2a1 *undocummented (part_id + pnr loc wanted)
		 * ra4m1 *undocummented (part_id + pnr loc wanted)
		 * ra4m2 *undocummented (part_id + pnr loc wanted)
		 * ra4w1 *undocummented (part_id + pnr loc wanted)
		 */

		if (renesas_pnr_read(t, RENESAS_FIXED2_PNR, pnr)) {
			DEBUG_WARN("Found renesas chip (%.*s) with pnr location RENESAS_FIXED2_PNR and unsupported Part ID %" PRIx16
					   " please report it\n",
				sizeof(pnr), pnr, t->part_id);
			break;
		}

		if (renesas_pnr_read(t, RENESAS_FIXED1_PNR, pnr)) {
			DEBUG_WARN("Found renesas chip (%.*s) with pnr location RENESAS_FIXED1_PNR and unsupported Part ID "
					   "0x%" PRIx16 " please report it\n",
				sizeof(pnr), pnr, t->part_id);
			break;
		}

		flash_root_table = renesas_fmifrt_read(t);
		if (renesas_pnr_read(t, RENESAS_FMIFRT_PNR(flash_root_table), pnr)) {
			DEBUG_WARN("Found renesas chip (%.*s) with Flash Root Table and unsupported Part ID 0x%" PRIx16 " "
					   "please report it\n",
				sizeof(pnr), pnr, t->part_id);
			break;
		}

		return false;
	}

	renesas_priv_s *priv_storage = calloc(1, sizeof(renesas_priv_s));
	if (!priv_storage) /* calloc failed: heap exhaustion */
		return false;
	memcpy(priv_storage->pnr, pnr, sizeof(pnr));

	priv_storage->series = renesas_series(pnr);
	priv_storage->flash_root_table = flash_root_table;

	t->target_storage = (void *)priv_storage;
	t->driver = (char *)priv_storage->pnr;

	switch (priv_storage->series) {
	case PNR_SERIES_RA2L1:
	case PNR_SERIES_RA2A1:
	case PNR_SERIES_RA4M1:
		renesas_add_flash(t, 0x40100000, 8UL * 1024UL, 64); /* Data flash memory 8 KB 0x40100000 */
		target_add_ram(t, 0x20000000, 32UL * 1024UL); /* SRAM 32 KB 0x20000000 */
		break;

	case PNR_SERIES_RA2E1:
		renesas_add_flash(t, 0x40100000, 4UL * 1024UL, 64); /* Data flash memory 4 KB 0x40100000 */
		target_add_ram(t, 0x20004000, 16UL * 1024UL); /* SRAM 16 KB 0x20004000 */
		break;

	case PNR_SERIES_RA2E2:
		renesas_add_flash(t, 0x40100000, 2UL * 1024UL, 64); /* Data flash memory 2 KB 0x40100000 */
		target_add_ram(t, 0x20004000, 8UL * 1024UL); /* SRAM 8 KB 0x20004000 */
		break;

	case PNR_SERIES_RA4M2:
	case PNR_SERIES_RA4M3:
	case PNR_SERIES_RA4E1:
		renesas_add_flash(t, 0x08000000, 8UL * 1024UL, 64); /* Data flash memory 8 KB 0x08000000 */
		target_add_ram(t, 0x20000000, 128UL * 1024UL); /* SRAM 128 KB 0x20000000 */
		target_add_ram(t, 0x28000000, 1024UL);         /* Standby SRAM 1 KB 0x28000000 */
		break;

	case PNR_SERIES_RA4W1:
		renesas_add_flash(t, 0x40100000, 8UL * 1024UL, 64); /* Data flash memory 8 KB 0x40100000 */
		target_add_ram(t, 0x20000000, 96UL * 1024UL); /* SRAM 96 KB 0x20000000 */
		break;

	case PNR_SERIES_RA6M1:
		/* conflicting information in the datasheet, here be dragons */
		renesas_add_flash(t, 0x40100000, 8UL * 1024UL, 64); /* Data flash memory 8 KB 0x40100000 */
		target_add_ram(t, 0x20000000, 128UL * 1024UL); /* SRAM 128 KB 0x20000000 */
		target_add_ram(t, 0x1FFE0000, 128UL * 1024UL); /* SRAMHS 128 KB 0x1FFE0000 */
		target_add_ram(t, 0x200FE000, 8UL * 1024UL);   /* Standby SRAM 8 KB 0x200FE000 */
		break;

	case PNR_SERIES_RA6M2:
		renesas_add_flash(t, 0x40100000, 32UL * 1024UL, 64); /* Data flash memory 32 KB 0x40100000 */
		target_add_ram(t, 0x20000000, 256UL * 1024UL); /* SRAM 256 KB 0x20000000 */
		target_add_ram(t, 0x1FFE0000, 128UL * 1024UL); /* SRAMHS 128 KB 0x1FFE0000 */
		target_add_ram(t, 0x200FE000, 8UL * 1024UL);   /* Standby SRAM 8 KB 0x200FE000 */
		break;

	case PNR_SERIES_RA6M3:
		renesas_add_flash(t, 0x40100000, 64UL * 1024UL, 64); /* Data flash memory 64 KB 0x40100000 */
		target_add_ram(t, 0x20000000, 256UL * 1024UL); /* SRAM0 256 KB 0x20000000 */
		target_add_ram(t, 0x20040000, 256UL * 1024UL); /* SRAM1 256 KB 0x20040000 */
		target_add_ram(t, 0x1FFE0000, 128UL * 1024UL); /* SRAMHS 128 KB 0x1FFE0000 */
		target_add_ram(t, 0x200FE000, 8UL * 1024UL);   /* Standby SRAM 8 KB 0x200FE000 */
		break;

	case PNR_SERIES_RA6M4:
	case PNR_SERIES_RA6E1:
		renesas_add_flash(t, 0x08000000, 8UL * 1024UL, 64); /* Data flash memory 8 KB 0x08000000 */
		target_add_ram(t, 0x20000000, 256UL * 1024UL); /* SRAM 256 KB 0x20000000 */
		target_add_ram(t, 0x28000000, 1024UL);         /* Standby SRAM 1 KB 0x28000000 */
		break;

	case PNR_SERIES_RA6M5:
		renesas_add_flash(t, 0x08000000, 8UL * 1024UL, 64); /* Data flash memory 8 KB 0x08000000 */
		target_add_ram(t, 0x20000000, 512UL * 1024UL); /* SRAM 512 KB 0x20000000 */
		target_add_ram(t, 0x28000000, 1024UL);         /* Standby SRAM 1 KB 0x28000000 */
		break;

	case PNR_SERIES_RA6T1:
		renesas_add_flash(t, 0x40100000, 8UL * 1024UL, 64); /* Data flash memory 8 KB 0x40100000 */
		target_add_ram(t, 0x1FFE0000, 64UL * 1024UL); /* SRAMHS 64 KB 0x1FFE0000 */
		break;

	case PNR_SERIES_RA6T2:
		renesas_add_flash(t, 0x08000000, 16UL * 1024UL, 64); /* Data flash memory 16 KB 0x08000000 */
		target_add_ram(t, 0x20000000, 64UL * 1024UL); /* SRAM 64 KB 0x20000000 */
		target_add_ram(t, 0x28000000, 1024UL);        /* Standby SRAM 1 KB 0x28000000 */
		break;

	default:
		return false;
	}

	renesas_add_flash(t, 0x00000000, renesas_flash_size(pnr), 8UL * 1024UL); /* Code flash memory 0x00000000 */

	target_add_commands(t, renesas_cmd_list, t->driver);

	return true;
}

/* Reads the 16-byte unique number */
static bool renesas_uid(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	renesas_priv_s *priv_storage = (renesas_priv_s *)t->target_storage;
	if (!priv_storage)
		return false;

	uint8_t uid[16];
	uint32_t uid_addr;

	switch (priv_storage->series) {
	case PNR_SERIES_RA2L1:
	case PNR_SERIES_RA2E1:
	case PNR_SERIES_RA2E2:
		uid_addr = RENESAS_FIXED1_UID;
		break;

	case PNR_SERIES_RA2A1:
	case PNR_SERIES_RA4M1:
	case PNR_SERIES_RA4M2:
	case PNR_SERIES_RA4M3:
	case PNR_SERIES_RA4E1:
	case PNR_SERIES_RA4W1:
	case PNR_SERIES_RA6M4:
	case PNR_SERIES_RA6M5:
	case PNR_SERIES_RA6E1:
	case PNR_SERIES_RA6T2:
		uid_addr = RENESAS_FIXED2_UID;
		break;

	case PNR_SERIES_RA6M1:
	case PNR_SERIES_RA6M2:
	case PNR_SERIES_RA6M3:
	case PNR_SERIES_RA6T1:
		uid_addr = RENESAS_FMIFRT_UID(priv_storage->flash_root_table);
		break;

	default:
		return false;
	}

	renesas_uid_read(t, uid_addr, uid);

	tc_printf(t, "Unique Number: 0x");
	for (size_t i = 0U; i < 16U; i++)
		tc_printf(t, "%02" PRIx8, uid[i]);
	tc_printf(t, "\n");

	return true;
}