/* ROM profile system for the Pokemon overlay.
 * Provides configurable table offsets and limits per ROM,
 * enabling future support for ROM hacks that relocate data.
 */

#ifndef ROMPROFILE_H
#define ROMPROFILE_H

#include <stdint.h>

struct RomProfile {
	const char* name;

	/* Limits */
	uint16_t speciesCount;
	uint16_t moveCount;

	/* ROM table offsets */
	uint32_t spriteTable;     /* front sprite pointer table (8 bytes/entry) */
	uint32_t paletteTable;    /* normal palette pointer table (8 bytes/entry) */
	uint32_t speciesNames;    /* species name table */
	uint32_t moveNames;       /* move name table */
	uint32_t learnsetTable;   /* level-up learnset pointer table */

	/* ROM table entry sizes */
	uint8_t speciesNameLen;
	uint8_t moveNameLen;

	/* EWRAM offsets (from WRAM base) */
	uint32_t partyCount;
	uint32_t partyData;

	/* Trainer table (for gym leader ROM reads) */
	uint32_t trainerTable;     /* ROM offset of gTrainers[] (40 bytes/entry) */
	uint16_t gymLeaderIds[8];  /* trainer IDs for each gym, badge order */

	/* Badge reading: SaveBlock1 pointer lives in IWRAM */
	uint32_t sb1PtrIwram;      /* IWRAM offset of gSaveBlock1Ptr */
	uint32_t sb1BadgeOffset;   /* offset from SB1 base to badge flags byte */

	/* Battle system (EWRAM offsets from WRAM base) */
	uint32_t battleFlags;      /* gBattleTypeFlags (u32, non-zero = in battle) */
	uint32_t battleMons;       /* gBattleMons[] (4 entries × 0x58 bytes each) */
	uint32_t currentMove;      /* gCurrentMove (u16) */
	uint32_t battlerAttacker;  /* gBattlerAttacker (u8, 0=player 1=opponent) */
};

/* Call once when ROM is available. Matches ROM header and sets active profile.
 * Returns 1 if a known profile matched, 0 if using default fallback. */
int romprofileDetect(const uint8_t* rom);

/* Get the active profile (never NULL — returns default if undetected) */
const struct RomProfile* romprofileGet(void);

#endif
