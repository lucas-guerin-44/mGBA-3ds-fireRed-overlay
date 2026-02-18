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
	},
};

#define PROFILE_COUNT (sizeof(sProfiles) / sizeof(sProfiles[0]))

static const struct RomProfile* sActive = &sProfiles[0];

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
		return 1;
	}

	/* Future: add more profiles here, or CRC32 matching for ROM hacks
	 * that share the same game code as their base ROM. */
	(void)i;

	/* Fallback to vanilla FireRed */
	sActive = &sProfiles[0];
	return 0;
}

const struct RomProfile* romprofileGet(void) {
	return sActive;
}
