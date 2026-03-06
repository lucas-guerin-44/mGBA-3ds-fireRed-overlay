/* ROM profile detection and lookup.
 * Matches the loaded ROM against a table of known profiles
 * and exposes the active profile for overlay + sprite code.
 */

#include "romprofile.h"
#include <string.h>

/* ===================================================================
 *  Known ROM profiles
 * =================================================================== */
static const struct RomProfile sProfiles[] = {
	{
		/* Pokemon FireRed US v1.0 (vanilla) */
		"FireRed US v1.0",
		412,        /* speciesCount */
		355,        /* moveCount */
		0x2350AC,   /* spriteTable */
		0x23730C,   /* paletteTable */
		0x245EE0,   /* speciesNames */
		0x247094,   /* moveNames */
		0x25D7B4,   /* learnsetTable */
		11,         /* speciesNameLen */
		13,         /* moveNameLen */
		0x24029,    /* partyCount */
		0x24284,    /* partyData */
		0x23EAC8,   /* trainerTable (gTrainers[], 40 bytes/entry, 743 entries) */
		{ 414, 415, 416, 417, 418, 420, 419, 350 }, /* gymLeaderIds (badge order) */
		0x5008,     /* sb1PtrIwram (gSaveBlock1Ptr at 0x03005008) */
		0x0FE4,     /* sb1BadgeOffset (flags@0x0EE0 + badge byte@0x104) */
		/* Battle system (from pokefirered decomp, confirmed relative to gPlayerParty) */
		0x22B4C,    /* battleFlags (gBattleTypeFlags at 0x02022B4C) */
		0x23BE4,    /* battleMons (gBattleMons at 0x02023BE4, 4 × 0x58) */
		0x23D4A,    /* currentMove (gCurrentMove at 0x02023D4A) */
		0x23D6B,    /* battlerAttacker (gBattlerAttacker at 0x02023D6B) */
	},
	{
		/* Pokemon Emerald US v1.0 (vanilla) */
		"Emerald US v1.0",
		412,        /* speciesCount (NUM_SPECIES, same as FireRed) */
		355,        /* moveCount (MOVES_COUNT) */
		0x301418,   /* spriteTable (gMonFrontPicTable) */
		0x303678,   /* paletteTable (gMonPaletteTable) */
		0x3185C8,   /* speciesNames (gSpeciesNames) */
		0x31977C,   /* moveNames (gMoveNames) */
		0x32937C,   /* learnsetTable (gLevelUpLearnsets) */
		11,         /* speciesNameLen (same Gen 3 format) */
		13,         /* moveNameLen (same Gen 3 format) */
		0x244E9,    /* partyCount (gPlayerPartyCount at 0x020244E9) */
		0x24744,    /* partyData (gPlayerParty at 0x02024744) */
		0x310030,   /* trainerTable (gTrainers[], 40 bytes/entry) */
		{ 265, 266, 267, 268, 269, 270, 271, 272 }, /* gymLeaderIds: Roxanne,Brawly,Wattson,Flannery,Norman,Winona,Tate&Liza,Juan */
		0x5D8C,     /* sb1PtrIwram (gSaveBlock1Ptr at 0x03005D8C) */
		0x137C,     /* sb1BadgeOffset (flags@0x1270 + badge byte@0x10C, FLAG_BADGE01_GET=0x867) */
		/* Battle system (from pokeemerald decomp symbols branch) */
		0x22FF0,    /* battleFlags (gBattleTypeFlags at 0x02022FF0) */
		0x24084,    /* battleMons (gBattleMons at 0x02024084, 4 × 0x58) */
		0x241EA,    /* currentMove (gCurrentMove at 0x020241EA) */
		0x2420B,    /* battlerAttacker (gBattlerAttacker at 0x0202420B) */
	},
};

#define PROFILE_COUNT (sizeof(sProfiles) / sizeof(sProfiles[0]))

static const struct RomProfile* sActive = &sProfiles[0];
static int sSupported = 0;

/* GBA ROM header: game code at offset 0xAC (4 bytes), version at 0xBC */
int romprofileDetect(const uint8_t* rom) {
	char gameCode[5];
	uint8_t version;
	int i;

	memcpy(gameCode, rom + 0xAC, 4);
	gameCode[4] = '\0';
	version = rom[0xBC];

	/* FireRed US v1.0: game code "BPRE", version 0 */
	if (memcmp(gameCode, "BPRE", 4) == 0 && version == 0) {
		sActive = &sProfiles[0];
		sSupported = 1;
		return 1;
	}

	/* Emerald US v1.0: game code "BPEE", version 0 */
	if (memcmp(gameCode, "BPEE", 4) == 0 && version == 0) {
		sActive = &sProfiles[1];
		sSupported = 1;
		return 1;
	}

	(void)i;

	sSupported = 0;
	return 0;
}

const struct RomProfile* romprofileGet(void) {
	return sActive;
}

int romprofileIsSupported(void) {
	return sSupported;
}

int romprofileGetCount(void) {
	return (int)PROFILE_COUNT;
}

const char* romprofileGetSlotName(int slot) {
	if (slot <= 0 || slot > (int)PROFILE_COUNT)
		return "Off";
	return sProfiles[slot - 1].name;
}

void romprofileSetSlot(int slot) {
	if (slot <= 0 || slot > (int)PROFILE_COUNT) {
		sSupported = 0;
	} else {
		sActive = &sProfiles[slot - 1];
		sSupported = 1;
	}
}

int romprofileGetSlot(void) {
	int i;
	if (!sSupported)
		return 0;
	for (i = 0; i < (int)PROFILE_COUNT; i++) {
		if (sActive == &sProfiles[i])
			return i + 1;
	}
	return 0;
}
