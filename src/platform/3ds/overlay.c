/* Pokemon FireRed overlay for mGBA 3DS
 * Reads party data from GBA EWRAM, decrypts Gen3 structures,
 * and renders a detail view per party member on the bottom screen.
 * Species/move names are decoded from ROM tables at runtime.
 * Sprites are decoded from ROM via sprite.c (LZ77 + 4bpp tiles).
 *
 * ZR/ZL cycles through party members.
 */

#include "overlay.h"
#include "sprite.h"
#include "romprofile.h"

#include <mgba/core/core.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#endif
#include "feature/gui/gui-runner.h"
#include <mgba-util/gui/font.h>
#include <string.h>
#include <stdio.h>
#include <3ds.h>

/* --- ABGR color constants (0xAABBGGRR) --- */
#define CLR_WHITE   0xFFFFFFFF
#define CLR_GREEN   0xFF40FF40  /* HP > 50% */
#define CLR_YELLOW  0xFF00FFFF  /* HP 25-50% */
#define CLR_RED     0xFF4040FF  /* HP < 25% */
#define CLR_GRAY    0xFFC0C0C0  /* secondary text */
#define CLR_DARK    0xFF808080  /* fainted / empty */
#define CLR_HEADER  0xFFFFFF60  /* section headers (cyan-ish) */
#define CLR_MOVE    0xFFE0E0FF  /* move names (warm white) */
#define CLR_STAT_UP 0xFF5050FF  /* red: nature-boosted stat (+10%) */
#define CLR_STAT_DN 0xFFFF8050  /* blue: nature-reduced stat (-10%) */

/* --- UI panel colors --- */
#define UI_PANEL    0xD0231919  /* #191923 dark charcoal, slightly transparent */
#define UI_BORDER   0xFF585050  /* #505058 gray border */
#define UI_ACCENT   0xFF686060  /* #606068 lighter gray accent */

#define POKEMON_SLOT_SIZE 100
#define MAX_PARTY         6

/* --- Overlay state: current party slot index (0..5) --- */
static int sOverlayMode = 0;
static unsigned sPrevHeld = 0; /* for edge detection */
static int sShowLearnset = 0;   /* 0 = current moves, 1 = learnset */
static int sLearnsetScroll = 0; /* scroll offset within learnset */

/* ===================================================================
 *  Gen 3 substructure order table  (Bulbapedia canonical)
 *  Index = PID % 24
 *  Values: 0=Growth, 1=Attacks, 2=EVs, 3=Misc
 * =================================================================== */
static const uint8_t sSubstructOrder[24][4] = {
	/* 0  GAEM */ {0,1,2,3}, /* 1  GAME */ {0,1,3,2},
	/* 2  GEAM */ {0,2,1,3}, /* 3  GEMA */ {0,2,3,1},
	/* 4  GMAE */ {0,3,1,2}, /* 5  GMEA */ {0,3,2,1},
	/* 6  AGEM */ {1,0,2,3}, /* 7  AGME */ {1,0,3,2},
	/* 8  AEGM */ {1,2,0,3}, /* 9  AEMG */ {1,2,3,0},
	/* 10 AMGE */ {1,3,0,2}, /* 11 AMEG */ {1,3,2,0},
	/* 12 EGAM */ {2,0,1,3}, /* 13 EGMA */ {2,0,3,1},
	/* 14 EAGM */ {2,1,0,3}, /* 15 EAMG */ {2,1,3,0},
	/* 16 EMGA */ {2,3,0,1}, /* 17 EMAG */ {2,3,1,0},
	/* 18 MGAE */ {3,0,1,2}, /* 19 MGEA */ {3,0,2,1},
	/* 20 MAGE */ {3,1,0,2}, /* 21 MAEG */ {3,1,2,0},
	/* 22 MEGA */ {3,2,0,1}, /* 23 MEAG */ {3,2,1,0},
};

/* Forward declaration (defined below, needed by readGymLeaderInfo) */
static void decodeGen3String(const uint8_t* src, char* dst, int maxLen);

/* ===================================================================
 *  Gym leader info — read from ROM trainer table
 * =================================================================== */
struct GymLeaderInfo {
	char    name[13];   /* decoded trainer name */
	uint8_t aceLevel;   /* highest level in party */
};

static int readGymLeaderInfo(const uint8_t* rom, int badgeIndex,
                             struct GymLeaderInfo* out)
{
	const struct RomProfile* prof = romprofileGet();
	uint32_t entryOff, partyPtr, partyOff;
	uint8_t partyFlags, partySize;
	int monSize, i;

	if (badgeIndex < 0 || badgeIndex >= 8) return 0;

	entryOff = prof->trainerTable + prof->gymLeaderIds[badgeIndex] * 40;

	/* Trainer name: offset 0x04, 12 bytes Gen3-encoded */
	decodeGen3String(rom + entryOff + 0x04, out->name, 12);

	/* Party metadata */
	partyFlags = rom[entryOff];
	partySize  = rom[entryOff + 0x20];

	memcpy(&partyPtr, rom + entryOff + 0x24, 4);
	if ((partyPtr >> 24) != 0x08) return 0;
	partyOff = partyPtr & 0x01FFFFFF;

	/* Mon entry: 8 bytes default, 16 with custom moves (bit 0) */
	monSize = (partyFlags & 1) ? 16 : 8;

	/* Scan for highest level (level at +0x02 in each mon) */
	out->aceLevel = 0;
	for (i = 0; i < partySize && i < 6; i++) {
		uint8_t lvl = rom[partyOff + i * monSize + 2];
		if (lvl > out->aceLevel) out->aceLevel = lvl;
	}

	return 1;
}

/* ===================================================================
 *  Nature -> stat color helper
 *  boosted = nature/5, reduced = nature%5
 *  0=Atk 1=Def 2=Spe 3=SpA 4=SpD
 * =================================================================== */
static uint32_t natureStatColor(uint8_t nature, int statIdx) {
	int up = nature / 5, dn = nature % 5;
	if (up == dn) return CLR_WHITE;
	if (statIdx == up) return CLR_STAT_UP;
	if (statIdx == dn) return CLR_STAT_DN;
	return CLR_WHITE;
}

/* ===================================================================
 *  Badge reader — find next unearned badge (0-7), or -1 if all earned
 * =================================================================== */
static int readNextBadge(const uint8_t* wram, const uint8_t* iwram) {
	const struct RomProfile* prof = romprofileGet();
	uint32_t sb1Ptr;
	uint32_t sb1Off;
	uint8_t badges;
	int i;

	memcpy(&sb1Ptr, iwram + prof->sb1PtrIwram, 4);
	if ((sb1Ptr >> 24) != 0x02) return -1;
	sb1Off = sb1Ptr & 0x3FFFF;
	badges = wram[sb1Off + prof->sb1BadgeOffset];

	for (i = 0; i < 8; i++) {
		if (!(badges & (1 << i))) return i;
	}
	return -1;
}

/* ===================================================================
 *  Gen 3 character decoding (Pokemon text encoding -> ASCII)
 * =================================================================== */
static char decodeGen3Char(uint8_t c) {
	if (c >= 0xBB && c <= 0xD4) return 'A' + (c - 0xBB);
	if (c >= 0xD5 && c <= 0xEE) return 'a' + (c - 0xD5);
	if (c >= 0xA1 && c <= 0xAA) return '0' + (c - 0xA1);
	if (c == 0x00) return ' ';
	if (c == 0xAB) return '!';
	if (c == 0xAC) return '?';
	if (c == 0xAD) return '.';
	if (c == 0xAE) return '-';
	if (c == 0xB8) return ',';
	if (c == 0xBA) return '/';
	if (c == 0xFF) return '\0';
	return ' ';
}

static void decodeGen3String(const uint8_t* src, char* dst, int maxLen) {
	int i;
	for (i = 0; i < maxLen; i++) {
		dst[i] = decodeGen3Char(src[i]);
		if (dst[i] == '\0') return;
	}
	dst[maxLen] = '\0';
}

/* ===================================================================
 *  ROM name table readers
 * =================================================================== */
static void readSpeciesName(const uint8_t* rom, uint16_t species, char* buf) {
	const struct RomProfile* p = romprofileGet();
	if (species == 0 || species >= p->speciesCount) {
		strcpy(buf, "???");
		return;
	}
	decodeGen3String(rom + p->speciesNames + species * p->speciesNameLen,
	                 buf, p->speciesNameLen);
}

static void readMoveName(const uint8_t* rom, uint16_t moveId, char* buf) {
	const struct RomProfile* p = romprofileGet();
	if (moveId == 0) {
		strcpy(buf, "---");
		return;
	}
	if (moveId >= p->moveCount) {
		strcpy(buf, "???");
		return;
	}
	decodeGen3String(rom + p->moveNames + moveId * p->moveNameLen,
	                 buf, p->moveNameLen);
}

/* ===================================================================
 *  Gen 3 substructure decryption
 * =================================================================== */
static void decryptSubstructs(const uint8_t* pokemon, uint32_t key, uint8_t* out) {
	const uint32_t* enc = (const uint32_t*)(pokemon + 0x20);
	uint32_t* dec = (uint32_t*)out;
	int i;
	for (i = 0; i < 12; i++) {
		dec[i] = enc[i] ^ key;
	}
}

static int findSubstructOffset(uint32_t pid, int which) {
	int order = pid % 24;
	int pos;
	for (pos = 0; pos < 4; pos++) {
		if (sSubstructOrder[order][pos] == which) {
			return pos * 12;
		}
	}
	return 0;
}

/* ===================================================================
 *  HP color helper
 * =================================================================== */
static uint32_t hpColor(uint16_t cur, uint16_t max) {
	int pct;
	if (max == 0 || cur == 0) return CLR_DARK;
	pct = (cur * 100) / max;
	if (pct > 50) return CLR_GREEN;
	if (pct > 25) return CLR_YELLOW;
	return CLR_RED;
}

/* ===================================================================
 *  Status condition text helper
 * =================================================================== */
static const char* statusText(uint32_t status) {
	if (status == 0)    return NULL;
	if (status & 0x07)  return "SLP";
	if (status & 0x08)  return "PSN";
	if (status & 0x10)  return "BRN";
	if (status & 0x20)  return "FRZ";
	if (status & 0x40)  return "PAR";
	if (status & 0x80)  return "TOX";
	return NULL;
}

/* ===================================================================
 *  Shared: decode one party slot into useful fields
 * =================================================================== */
struct PokeSlot {
	uint16_t species;
	uint32_t exp;
	uint16_t moves[4];
	uint8_t  pp[4];
	uint8_t  level;
	uint8_t  nature;
	uint16_t curHP, maxHP;
	uint16_t atk, def, spe, spa, spd;
	uint8_t  ivHP, ivAtk, ivDef, ivSpe, ivSpA, ivSpD;
	uint8_t  evHP, evAtk, evDef, evSpe, evSpA, evSpD;
	uint32_t status;
	char     nickname[11];
	char     speciesName[12];
};

static int readSlot(const uint8_t* wram, const uint8_t* rom,
                    int index, struct PokeSlot* out) {
	const struct RomProfile* prof = romprofileGet();
	uint32_t base = prof->partyData + (index * POKEMON_SLOT_SIZE);
	const uint8_t* slot = wram + base;
	uint32_t pid, otid, key;
	uint8_t decrypted[48];
	int growthOff, attackOff, evsOff, miscOff;

	pid  = ((const uint32_t*)slot)[0];
	otid = ((const uint32_t*)slot)[1];
	key  = pid ^ otid;

	decryptSubstructs(slot, key, decrypted);

	/* Growth (type 0): species + EXP */
	growthOff = findSubstructOffset(pid, 0);
	memcpy(&out->species, decrypted + growthOff, 2);
	memcpy(&out->exp,     decrypted + growthOff + 4, 4);

	if (out->species == 0 || out->species >= prof->speciesCount)
		return 0; /* invalid */

	readSpeciesName(rom, out->species, out->speciesName);

	/* Attacks (type 1): 4 moves + 4 PP */
	attackOff = findSubstructOffset(pid, 1);
	memcpy(&out->moves[0], decrypted + attackOff + 0, 2);
	memcpy(&out->moves[1], decrypted + attackOff + 2, 2);
	memcpy(&out->moves[2], decrypted + attackOff + 4, 2);
	memcpy(&out->moves[3], decrypted + attackOff + 6, 2);
	out->pp[0] = decrypted[attackOff + 8];
	out->pp[1] = decrypted[attackOff + 9];
	out->pp[2] = decrypted[attackOff + 10];
	out->pp[3] = decrypted[attackOff + 11];

	/* Nickname (unencrypted header, offset 0x08, 10 bytes) */
	decodeGen3String(slot + 0x08, out->nickname, 10);

	/* Party stats (unencrypted, offset 0x50+) */
	out->level = slot[0x54];
	memcpy(&out->curHP, slot + 0x56, 2);
	memcpy(&out->maxHP, slot + 0x58, 2);
	memcpy(&out->atk,   slot + 0x5A, 2);
	memcpy(&out->def,   slot + 0x5C, 2);
	memcpy(&out->spe,   slot + 0x5E, 2);
	memcpy(&out->spa,   slot + 0x60, 2);
	memcpy(&out->spd,   slot + 0x62, 2);

	/* Nature (derived from PID) */
	out->nature = pid % 25;

	/* Status condition (unencrypted, offset 0x50) */
	memcpy(&out->status, slot + 0x50, 4);

	/* EVs (substructure type 2): HP/Atk/Def/Spe/SpA/SpD */
	evsOff = findSubstructOffset(pid, 2);
	out->evHP  = decrypted[evsOff + 0];
	out->evAtk = decrypted[evsOff + 1];
	out->evDef = decrypted[evsOff + 2];
	out->evSpe = decrypted[evsOff + 3];
	out->evSpA = decrypted[evsOff + 4];
	out->evSpD = decrypted[evsOff + 5];

	/* IVs (substructure type 3, packed in bytes 4-7) */
	miscOff = findSubstructOffset(pid, 3);
	{
		uint32_t ivData;
		memcpy(&ivData, decrypted + miscOff + 4, 4);
		out->ivHP  =  ivData        & 0x1F;
		out->ivAtk = (ivData >>  5) & 0x1F;
		out->ivDef = (ivData >> 10) & 0x1F;
		out->ivSpe = (ivData >> 15) & 0x1F;
		out->ivSpA = (ivData >> 20) & 0x1F;
		out->ivSpD = (ivData >> 25) & 0x1F;
	}

	return 1; /* valid */
}

/* ===================================================================
 *  Read level-up learnset from ROM for a given species
 * =================================================================== */
struct LearnsetEntry {
	uint8_t  level;
	uint16_t moveId;
};

static int readLearnset(const uint8_t* rom, uint16_t species,
                        struct LearnsetEntry* out, int maxEntries)
{
	const struct RomProfile* prof = romprofileGet();
	uint32_t ptrAddr, dataOff;
	int count = 0;

	if (species == 0 || species >= prof->speciesCount) return 0;

	/* Read the pointer from the species table */
	memcpy(&ptrAddr, rom + prof->learnsetTable + species * 4, 4);

	/* Validate GBA ROM pointer (0x08xxxxxx) and convert to offset */
	if ((ptrAddr >> 24) != 0x08) return 0;
	dataOff = ptrAddr & 0x01FFFFFF;

	/* Decode entries: bits 0-8 = moveId, bits 9-15 = level */
	while (count < maxEntries) {
		uint16_t raw;
		memcpy(&raw, rom + dataOff + count * 2, 2);
		if (raw == 0xFFFF) break;
		out[count].moveId = raw & 0x1FF;
		out[count].level  = (raw >> 9) & 0x7F;
		count++;
	}

	return count;
}

/* ===================================================================
 *  DETAIL VIEW — single Pokemon deep dive (PMD-style framed panels)
 * =================================================================== */
static void drawDetail(struct GUIFont* font, const uint8_t* wram,
                       const uint8_t* rom, int slotIndex, uint8_t partyCount,
                       int nextBadge,
                       int screenW, int padX, int padY, int lineH)
{
	struct PokeSlot pk;
	struct GymLeaderInfo gym;
	int hasGym;
	int y;
	int m;
	char moveNameBuf[14];
	struct LearnsetEntry learnset[32];
	int learnTotal, learnShown, li;
	int panelL, panelR, panelW, inset;
	int sprX, sprY, textX, textR;
	int topH;

	hasGym = (nextBadge >= 0) ? readGymLeaderInfo(rom, nextBadge, &gym) : 0;

	if (!readSlot(wram, rom, slotIndex, &pk)) {
		GUIFontPrintf(font, screenW / 2, screenW / 4,
		              GUI_ALIGN_HCENTER, CLR_DARK, "Empty slot");
		return;
	}

	panelL = padX;
	panelR = screenW - padX;
	panelW = panelR - panelL;
	inset  = 6;
	sprX   = panelL + inset;
	sprY   = padY + inset;
	textX  = sprX + 64 + 8;
	textR  = panelR - inset;

	/* === TOP PANEL: sprite + basic info === */
	topH = 64 + inset * 2;
	drawRect(panelL - 2, padY - 2, panelW + 4, topH + 4, UI_BORDER);
	drawRect(panelL, padY, panelW, topH, UI_PANEL);

	/* Sprite frame + sprite (top-left) */
	drawRect(sprX - 2, sprY - 2, 68, 68, UI_ACCENT);
	drawPokemonSprite(rom, pk.species, sprX, sprY, 1);

	/* Rows 1-3: vertically centered beside 64px sprite */
	y = sprY + (64 - lineH * 3) / 2;

	/* Row 1: species [status] + gym leader name */
	{
		const char* sts = statusText(pk.status);
		if (sts)
			GUIFontPrintf(font, textX, y, GUI_ALIGN_LEFT, CLR_HEADER,
			              "%s  [%s]", pk.speciesName, sts);
		else
			GUIFontPrintf(font, textX, y, GUI_ALIGN_LEFT, CLR_HEADER,
			              "%s", pk.speciesName);
	}
	if (hasGym)
		GUIFontPrintf(font, textR, y, GUI_ALIGN_RIGHT, CLR_GRAY,
		              "%s", gym.name);
	y += lineH;

	/* Row 2: level + gym leader ace level */
	GUIFontPrintf(font, textX, y, GUI_ALIGN_LEFT, CLR_WHITE,
	              "Lv.%u", pk.level);
	if (hasGym)
		GUIFontPrintf(font, textR, y, GUI_ALIGN_RIGHT, CLR_GRAY,
		              "Ace Lv%u", gym.aceLevel);
	y += lineH;

	/* Row 3: nickname + HP */
	GUIFontPrintf(font, textX, y, GUI_ALIGN_LEFT, CLR_GRAY,
	              "%s", pk.nickname);
	GUIFontPrintf(font, textR, y, GUI_ALIGN_RIGHT,
	              hpColor(pk.curHP, pk.maxHP),
	              "HP %u/%u", pk.curHP, pk.maxHP);

	/* === STATS PANEL (2 rows: stats left, IV/EV right) === */
	y = padY + topH + 6;
	{
		int statsH = lineH * 2 + 24;
		drawRect(panelL - 2, y - 2, panelW + 4, statsH + 4, UI_BORDER);
		drawRect(panelL, y, panelW, statsH, UI_PANEL);
		y += 12;

		{
			char sbuf[16];
			int sx = panelL + inset;

			snprintf(sbuf, sizeof(sbuf), "Atk:%-3u ", pk.atk);
			GUIFontPrintf(font, sx, y, GUI_ALIGN_LEFT, natureStatColor(pk.nature, 0), "%s", sbuf);
			sx += GUIFontSpanWidth(font, sbuf);

			snprintf(sbuf, sizeof(sbuf), "Def:%-3u ", pk.def);
			GUIFontPrintf(font, sx, y, GUI_ALIGN_LEFT, natureStatColor(pk.nature, 1), "%s", sbuf);
			sx += GUIFontSpanWidth(font, sbuf);

			GUIFontPrintf(font, sx, y, GUI_ALIGN_LEFT, natureStatColor(pk.nature, 2), "Spe:%u", pk.spe);
		}
		GUIFontPrintf(font, panelR - inset, y, GUI_ALIGN_RIGHT, CLR_GRAY,
		              "IV %u/%u/%u/%u/%u/%u",
		              pk.ivHP, pk.ivAtk, pk.ivDef, pk.ivSpe, pk.ivSpA, pk.ivSpD);
		y += lineH;

		{
			char sbuf[16];
			int sx = panelL + inset;

			snprintf(sbuf, sizeof(sbuf), "SpA:%-3u ", pk.spa);
			GUIFontPrintf(font, sx, y, GUI_ALIGN_LEFT, natureStatColor(pk.nature, 3), "%s", sbuf);
			sx += GUIFontSpanWidth(font, sbuf);

			GUIFontPrintf(font, sx, y, GUI_ALIGN_LEFT, natureStatColor(pk.nature, 4), "SpD:%u", pk.spd);
		}
		GUIFontPrintf(font, panelR - inset, y, GUI_ALIGN_RIGHT, CLR_GRAY,
		              "EV %u/%u/%u/%u/%u/%u",
		              pk.evHP, pk.evAtk, pk.evDef, pk.evSpe, pk.evSpA, pk.evSpD);
		y += lineH + 3;
	}

	/* === MOVES PANEL === */
	y += 4;
	{
		int movesY = y;
		int movesH = lineH * 5 + 26;
		drawRect(panelL - 2, movesY - 2, panelW + 4, movesH + 4, UI_BORDER);
		drawRect(panelL, movesY, panelW, movesH, UI_PANEL);
		y += 13;

		if (sShowLearnset) {
			GUIFontPrintf(font, panelL + inset, y, GUI_ALIGN_LEFT,
			              CLR_HEADER, "LEARNSET");
			GUIFontPrintf(font, panelR - inset, y, GUI_ALIGN_RIGHT,
			              CLR_DARK, "Y: scroll");
			y += lineH;

			learnTotal = readLearnset(rom, pk.species, learnset, 32);
			{
				int upcoming = 0;
				int skipped = 0;
				for (li = 0; li < learnTotal; li++) {
					if (learnset[li].level > pk.level) upcoming++;
				}
				if (upcoming <= 4)
					sLearnsetScroll = 0;
				else if (sLearnsetScroll > upcoming - 4)
					sLearnsetScroll = upcoming - 4;

				learnShown = 0;
				for (li = 0; li < learnTotal && learnShown < 4; li++) {
					if (learnset[li].level <= pk.level) continue;
					if (skipped < sLearnsetScroll) {
						skipped++;
						continue;
					}
					readMoveName(rom, learnset[li].moveId, moveNameBuf);
					GUIFontPrintf(font, panelL + inset + 4, y,
					              GUI_ALIGN_LEFT, CLR_MOVE,
					              "Lv.%-3u %s", learnset[li].level, moveNameBuf);
					y += lineH;
					learnShown++;
				}
			}
			if (learnShown == 0) {
				GUIFontPrintf(font, panelL + inset + 4, y,
				              GUI_ALIGN_LEFT, CLR_DARK, "(no more moves)");
			}
		} else {
			GUIFontPrintf(font, panelL + inset, y, GUI_ALIGN_LEFT,
			              CLR_HEADER, "MOVES");
			y += lineH;

			for (m = 0; m < 4; m++) {
				if (pk.moves[m] == 0) continue;
				readMoveName(rom, pk.moves[m], moveNameBuf);
				GUIFontPrintf(font, panelL + inset + 4, y,
				              GUI_ALIGN_LEFT, CLR_MOVE, "%s", moveNameBuf);
				GUIFontPrintf(font, panelR - inset, y,
				              GUI_ALIGN_RIGHT, CLR_GRAY, "PP:%u", pk.pp[m]);
				y += lineH;
			}
		}
	}

	/* === FOOTER (outside panels) === */
	GUIFontPrintf(font, panelL, 240 - padY, GUI_ALIGN_LEFT, CLR_DARK,
	              "[%d/%u]", slotIndex + 1, partyCount);
	GUIFontPrintf(font, panelR, 240 - padY, GUI_ALIGN_RIGHT,
	              CLR_HEADER, sShowLearnset ? "[Moves]" : "[Learnset]");
}

/* ===================================================================
 *  Main entry — called from _drawOverlay in main.c
 * =================================================================== */
void overlayDraw(struct mGUIRunner* runner, struct GUIFont* font,
                 int screenW, int screenH, unsigned keysDown)
{
#ifdef M_CORE_GBA
	struct GBA* gba;
	uint8_t* wram;
	uint8_t* rom;
	uint8_t* iwram;
	uint8_t partyCount;
	int nextBadge;
	int lineH, padX, padY;

	(void)screenH;

	if (!runner->core || !runner->core->board) {
		GUIFontPrintf(font, screenW / 2, screenH / 2,
		              GUI_ALIGN_HCENTER, CLR_DARK, "No ROM loaded");
		return;
	}

	gba = (struct GBA*) runner->core->board;
	wram = (uint8_t*) gba->memory.wram;
	rom  = (uint8_t*) gba->memory.rom;
	iwram = (uint8_t*) gba->memory.iwram;

	{ /* Detect ROM profile once */
		static int sProfileDetected = 0;
		if (!sProfileDetected) {
			romprofileDetect(rom);
			sProfileDetected = 1;
		}
	}

	padX = 12;
	padY = 8;
	lineH = GUIFontHeight(font) + 2;

	partyCount = wram[romprofileGet()->partyCount];
	if (partyCount > MAX_PARTY) partyCount = MAX_PARTY;
	nextBadge = readNextBadge(wram, iwram);

	/* --- Input: direct poll with edge detection --- */
	{
		unsigned held = hidKeysHeld();
		unsigned pressed = held & ~sPrevHeld; /* newly pressed this frame */
		sPrevHeld = held;

		if (pressed & KEY_ZR) {
			sOverlayMode++;
			if (sOverlayMode >= partyCount)
				sOverlayMode = 0;
			sLearnsetScroll = 0;
		}
		if (pressed & KEY_ZL) {
			sOverlayMode--;
			if (sOverlayMode < 0)
				sOverlayMode = partyCount - 1;
			sLearnsetScroll = 0;
		}
		if (pressed & KEY_TOUCH) {
			touchPosition touch;
			hidTouchRead(&touch);
			if (touch.px > (unsigned)(screenW / 2) && touch.py > 200) {
				sShowLearnset = !sShowLearnset;
				sLearnsetScroll = 0;
			} else {
				sOverlayMode++;
				if (sOverlayMode >= partyCount)
					sOverlayMode = 0;
				sLearnsetScroll = 0;
			}
		}

		if (pressed & KEY_CPAD_RIGHT) {
			sOverlayMode++;
			if (sOverlayMode >= partyCount)
				sOverlayMode = 0;
			sLearnsetScroll = 0;
		}
		if (pressed & KEY_CPAD_LEFT) {
			sOverlayMode--;
			if (sOverlayMode < 0)
				sOverlayMode = partyCount - 1;
			sLearnsetScroll = 0;
		}
		if (pressed & KEY_CPAD_DOWN) {
			sLearnsetScroll++;
		}
		if (pressed & KEY_CPAD_UP) {
			if (sLearnsetScroll > 0)
				sLearnsetScroll--;
		}
	}
	/* Clamp if party shrank */
	if (sOverlayMode >= partyCount)
		sOverlayMode = 0;

	/* --- Always detail view --- */
	drawDetail(font, wram, rom, sOverlayMode, partyCount,
	           nextBadge, screenW, padX, padY, lineH);
#else
	(void)runner;
	GUIFontPrintf(font, screenW / 2, screenH / 2,
	              GUI_ALIGN_HCENTER, CLR_DARK, "GBA core not available");
#endif
}
