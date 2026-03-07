/* walker.c — Pokewalker-style mon transfer via QR codes.
 *
 * SEND flow: read a 100-byte party slot from EWRAM, base64-encode it,
 *   generate a QR code via qrcodegen, display it on the bottom screen.
 *   On confirm, zero the slot and compact the party.
 *
 * RECEIVE flow: handled in qrscan.c (PK2:RECV payload parsing).
 */

#include "walker.h"
#include "romprofile.h"
#include "sprite.h"
#include "ctr-gpu.h"
#include "qrcodegen/qrcodegen.h"

#include <mgba/core/core.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#endif
#include "feature/gui/gui-runner.h"

#include <3ds.h>
#include <string.h>
#include <stdio.h>

/* --- ABGR colors --- */
#define CLR_WHITE   0xFFFFFFFF
#define CLR_BLACK   0xFF000000
#define CLR_GREEN   0xFF40FF40
#define CLR_RED     0xFF4040FF
#define CLR_GRAY    0xFFC0C0C0
#define CLR_CYAN    0xFFFFFF60
#define UI_PANEL    0xD0231919

#define PARTY_SLOT_SIZE 100

/* ===================================================================
 *  Base64 encode/decode
 * =================================================================== */
static const char b64enc[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Decode: returns 0-63 for valid chars, 0xFF for invalid */
static uint8_t b64val(char c) {
	if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A');
	if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 26);
	if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 52);
	if (c == '+') return 62;
	if (c == '/') return 63;
	return 0xFF;
}

/* Encode src (srcLen bytes) into dst as base64. Returns output length.
 * dst must hold at least ((srcLen + 2) / 3) * 4 + 1 bytes. */
static int base64Encode(const uint8_t* src, int srcLen, char* dst) {
	int i, o = 0;
	for (i = 0; i < srcLen - 2; i += 3) {
		dst[o++] = b64enc[(src[i] >> 2) & 0x3F];
		dst[o++] = b64enc[((src[i] & 0x03) << 4) | (src[i+1] >> 4)];
		dst[o++] = b64enc[((src[i+1] & 0x0F) << 2) | (src[i+2] >> 6)];
		dst[o++] = b64enc[src[i+2] & 0x3F];
	}
	if (i < srcLen) {
		dst[o++] = b64enc[(src[i] >> 2) & 0x3F];
		if (i + 1 < srcLen) {
			dst[o++] = b64enc[((src[i] & 0x03) << 4) | (src[i+1] >> 4)];
			dst[o++] = b64enc[(src[i+1] & 0x0F) << 2];
		} else {
			dst[o++] = b64enc[(src[i] & 0x03) << 4];
			dst[o++] = '=';
		}
		dst[o++] = '=';
	}
	dst[o] = '\0';
	return o;
}

/* Decode base64 src into dst. Returns number of bytes decoded, or -1 on error.
 * dst must hold at least (strlen(src) * 3 / 4) bytes. */
static int base64Decode(const char* src, uint8_t* dst, int dstMax) {
	int len = (int)strlen(src);
	int i, o = 0;
	uint32_t acc = 0;
	int bits = 0;

	for (i = 0; i < len; i++) {
		uint8_t val;
		if (src[i] == '=') break;
		if (src[i] == '\n' || src[i] == '\r' || src[i] == ' ') continue;
		val = b64val(src[i]);
		if (val == 0xFF) return -1;
		acc = (acc << 6) | val;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= dstMax) return -1;
			dst[o++] = (uint8_t)(acc >> bits);
		}
	}
	return o;
}

/* ===================================================================
 *  State
 * =================================================================== */
static bool sActive = false;
static int sSendSlot = -1;              /* party slot being sent */
static uint8_t sMonBlob[PARTY_SLOT_SIZE]; /* copy of the mon data */
static char sPayload[256];              /* "PK2:SEND:<base64>" */
static int sPayloadLen = 0;

/* QR code modules (max version ~10 = 57 modules, but allow up to version 20) */
#define QR_MAX_MODULES 100
static uint8_t sQrModules[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
static uint8_t sQrTempBuf[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
static int sQrSize = 0; /* modules per side */

static char sNickname[12];
static char sSpeciesName[12];
static uint8_t sLevel;

/* Gen3 charset → ASCII (simplified, covers common chars) */
static const char gen3Charset[256] = {
	[0x00]=' ', [0xBB]='A', [0xBC]='B', [0xBD]='C', [0xBE]='D', [0xBF]='E',
	[0xC0]='F', [0xC1]='G', [0xC2]='H', [0xC3]='I', [0xC4]='J', [0xC5]='K',
	[0xC6]='L', [0xC7]='M', [0xC8]='N', [0xC9]='O', [0xCA]='P', [0xCB]='Q',
	[0xCC]='R', [0xCD]='S', [0xCE]='T', [0xCF]='U', [0xD0]='V', [0xD1]='W',
	[0xD2]='X', [0xD3]='Y', [0xD4]='Z', [0xD5]='a', [0xD6]='b', [0xD7]='c',
	[0xD8]='d', [0xD9]='e', [0xDA]='f', [0xDB]='g', [0xDC]='h', [0xDD]='i',
	[0xDE]='j', [0xDF]='k', [0xE0]='l', [0xE1]='m', [0xE2]='n', [0xE3]='o',
	[0xE4]='p', [0xE5]='q', [0xE6]='r', [0xE7]='s', [0xE8]='t', [0xE9]='u',
	[0xEA]='v', [0xEB]='w', [0xEC]='x', [0xED]='y', [0xEE]='z',
	[0xFF]='\0',
};

static void decodeGen3String(const uint8_t* src, int maxLen, char* dst) {
	int i;
	for (i = 0; i < maxLen; i++) {
		if (src[i] == 0xFF) break;
		dst[i] = gen3Charset[src[i]];
		if (dst[i] == '\0') dst[i] = '?';
	}
	dst[i] = '\0';
}

/* ===================================================================
 *  Init
 * =================================================================== */
void walkerInit(void) {
	sActive = false;
	sSendSlot = -1;
	sQrSize = 0;
}

/* ===================================================================
 *  Send flow
 * =================================================================== */
int walkerStartSend(struct mGUIRunner* runner, int slot) {
#ifdef M_CORE_GBA
	struct GBA* gba;
	uint8_t* wram;
	const uint8_t* rom;
	const struct RomProfile* prof;
	int partyCount;
	char b64[200]; /* 100 bytes -> 136 base64 chars + null */
	bool ok;

	if (!runner || !runner->core || !runner->core->board) return 0;
	if (!romprofileIsSupported()) return 0;

	gba = (struct GBA*) runner->core->board;
	wram = (uint8_t*) gba->memory.wram;
	rom = (const uint8_t*) gba->memory.rom;
	prof = romprofileGet();

	partyCount = wram[prof->partyCount];
	if (partyCount > 6) partyCount = 6;
	if (partyCount <= 1) return 0; /* can't send last mon */
	if (slot < 0 || slot >= partyCount) return 0;

	/* Copy the 100-byte party slot */
	memcpy(sMonBlob, wram + prof->partyData + slot * PARTY_SLOT_SIZE,
	       PARTY_SLOT_SIZE);

	/* Extract display info */
	decodeGen3String(sMonBlob + 0x08, 10, sNickname);
	sLevel = sMonBlob[0x54];

	/* Read species name from ROM */
	{
		uint16_t species;
		uint32_t pid, otid, key;
		uint8_t dec[48];
		int growthOff, i;
		/* Substructure order table */
		static const uint8_t subOrder[24][4] = {
			{0,1,2,3}, {0,1,3,2}, {0,2,1,3}, {0,2,3,1},
			{0,3,1,2}, {0,3,2,1}, {1,0,2,3}, {1,0,3,2},
			{1,2,0,3}, {1,2,3,0}, {1,3,0,2}, {1,3,2,0},
			{2,0,1,3}, {2,0,3,1}, {2,1,0,3}, {2,1,3,0},
			{2,3,0,1}, {2,3,1,0}, {3,0,1,2}, {3,0,2,1},
			{3,1,0,2}, {3,1,2,0}, {3,2,0,1}, {3,2,1,0},
		};

		memcpy(&pid, sMonBlob, 4);
		memcpy(&otid, sMonBlob + 4, 4);
		key = pid ^ otid;

		/* Decrypt substructures */
		{
			const uint32_t* enc = (const uint32_t*)(sMonBlob + 0x20);
			uint32_t* d = (uint32_t*)dec;
			for (i = 0; i < 12; i++)
				d[i] = enc[i] ^ key;
		}

		/* Find Growth substructure, read species */
		growthOff = 0;
		{
			int order = pid % 24;
			int pos;
			for (pos = 0; pos < 4; pos++) {
				if (subOrder[order][pos] == 0) {
					growthOff = pos * 12;
					break;
				}
			}
		}
		memcpy(&species, dec + growthOff, 2);

		/* Read species name from ROM table */
		if (prof->speciesNames && species < prof->speciesCount) {
			decodeGen3String(rom + prof->speciesNames + species * prof->speciesNameLen,
			                 prof->speciesNameLen, sSpeciesName);
		} else {
			snprintf(sSpeciesName, sizeof(sSpeciesName), "#%u", species);
		}
	}

	/* Build payload: PK2:SEND:<otid_hex>:<base64>
	 * Include OTID so the phone can mint caught mons with the player's trainer ID */
	{
		uint32_t otid;
		memcpy(&otid, sMonBlob + 4, 4);
		base64Encode(sMonBlob, PARTY_SLOT_SIZE, b64);
		sPayloadLen = snprintf(sPayload, sizeof(sPayload),
		                       "PK2:SEND:%08lX:%s",
		                       (unsigned long)otid, b64);
	}

	/* Generate QR code */
	ok = qrcodegen_encodeText(sPayload,
		sQrTempBuf, sQrModules,
		qrcodegen_Ecc_LOW,
		qrcodegen_VERSION_MIN, 10,
		qrcodegen_Mask_AUTO, true);

	if (!ok) return 0;

	sQrSize = qrcodegen_getSize(sQrModules);
	sSendSlot = slot;
	sActive = true;
	return 1;
#else
	(void)runner; (void)slot;
	return 0;
#endif
}

int walkerIsActive(void) {
	return sActive ? 1 : 0;
}

/* ===================================================================
 *  Draw QR code on screen
 * =================================================================== */
void walkerDraw(struct GUIFont* font, int screenW, int screenH) {
	int qrPixelSize, qrTotalSize, qrX, qrY;
	int x, y, quietZone;
	(void)screenH;

	/* Background */
	drawRect(0, 0, screenW, 240, UI_PANEL);

	/* Header */
	GUIFontPrintf(font, screenW / 2, 4, GUI_ALIGN_HCENTER, CLR_CYAN,
	              "Sending %s (Lv.%u)", sSpeciesName, sLevel);
	GUIFontPrintf(font, screenW / 2, 16, GUI_ALIGN_HCENTER, CLR_GRAY,
	              "\"%s\"", sNickname);

	if (sQrSize <= 0) {
		GUIFontPrintf(font, screenW / 2, 120, GUI_ALIGN_HCENTER, CLR_RED,
		              "QR generation failed");
		return;
	}

	/* Calculate QR display size: fit in ~180px with quiet zone */
	quietZone = 2; /* 2-module quiet zone on each side */
	qrPixelSize = 180 / (sQrSize + quietZone * 2);
	if (qrPixelSize < 1) qrPixelSize = 1;
	if (qrPixelSize > 4) qrPixelSize = 4;
	qrTotalSize = (sQrSize + quietZone * 2) * qrPixelSize;
	qrX = (screenW - qrTotalSize) / 2;
	qrY = 30;

	/* White background (quiet zone) */
	drawRect(qrX, qrY, qrTotalSize, qrTotalSize, CLR_WHITE);

	/* Draw QR modules */
	for (y = 0; y < sQrSize; y++) {
		for (x = 0; x < sQrSize; x++) {
			if (qrcodegen_getModule(sQrModules, x, y)) {
				drawRect(qrX + (x + quietZone) * qrPixelSize,
				         qrY + (y + quietZone) * qrPixelSize,
				         qrPixelSize, qrPixelSize, CLR_BLACK);
			}
		}
	}

	/* Instructions */
	GUIFontPrintf(font, screenW / 2, qrY + qrTotalSize + 6,
	              GUI_ALIGN_HCENTER, CLR_WHITE,
	              "Scan with phone to receive mon");
	GUIFontPrintf(font, screenW / 2, 226,
	              GUI_ALIGN_HCENTER, CLR_GRAY,
	              "[A] Confirm sent   [B] Cancel");
}

/* ===================================================================
 *  Input polling
 * =================================================================== */
int walkerPoll(uint32_t keysDown) {
	if (!sActive) return 0;
	if (keysDown & KEY_A) return 1;  /* confirmed */
	if (keysDown & KEY_B) return -1; /* cancelled */
	return 0;
}

/* ===================================================================
 *  Confirm send: remove mon from party, compact
 * =================================================================== */
int walkerConfirmSend(struct mGUIRunner* runner) {
#ifdef M_CORE_GBA
	struct GBA* gba;
	uint8_t* wram;
	const struct RomProfile* prof;
	int partyCount, i;
	uint32_t partyBase;

	if (!sActive || sSendSlot < 0) return 0;
	if (!runner || !runner->core || !runner->core->board) return 0;

	gba = (struct GBA*) runner->core->board;
	wram = (uint8_t*) gba->memory.wram;
	prof = romprofileGet();

	partyCount = wram[prof->partyCount];
	if (partyCount > 6) partyCount = 6;
	if (sSendSlot >= partyCount) {
		sActive = false;
		return 0;
	}

	partyBase = prof->partyData;

	/* Shift slots down to fill the gap */
	for (i = sSendSlot; i < partyCount - 1; i++) {
		memcpy(wram + partyBase + i * PARTY_SLOT_SIZE,
		       wram + partyBase + (i + 1) * PARTY_SLOT_SIZE,
		       PARTY_SLOT_SIZE);
	}

	/* Zero out the last slot */
	memset(wram + partyBase + (partyCount - 1) * PARTY_SLOT_SIZE,
	       0, PARTY_SLOT_SIZE);

	/* Decrement party count */
	wram[prof->partyCount] = (uint8_t)(partyCount - 1);

	sActive = false;
	sSendSlot = -1;
	return 1;
#else
	(void)runner;
	return 0;
#endif
}

void walkerCancel(void) {
	sActive = false;
	sSendSlot = -1;
	sQrSize = 0;
}

/* ===================================================================
 *  Base64 decode — exposed for qrscan.c receive flow
 * =================================================================== */
int walkerBase64Decode(const char* src, uint8_t* dst, int dstMax) {
	return base64Decode(src, dst, dstMax);
}
