/* qrscan.c — QR code scanner using the outer (back-facing) camera.
 * Captures frames, converts to grayscale, feeds to quirc for decoding.
 * Draws a live camera preview + status on the overlay screen.
 */

#include "qrscan.h"
#include "walker.h"
#include "romprofile.h"
#include "sprite.h"
#include "ctr-gpu.h"
#include "quirc/quirc.h"
#include "quirc/quirc_internal.h"

#include <mgba/core/core.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#endif
#include "feature/gui/gui-runner.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* --- ABGR colors --- */
#define CLR_WHITE   0xFFFFFFFF
#define CLR_GREEN   0xFF40FF40
#define CLR_YELLOW  0xFF00FFFF
#define CLR_RED     0xFF4040FF
#define CLR_GRAY    0xFFC0C0C0
#define CLR_CYAN    0xFFFFFF60
#define UI_PANEL    0xD0231919

/* Camera captures at QVGA, downscaled 2x for quirc processing */
#define CAM_W 320
#define CAM_H 240
#define QR_W  160
#define QR_H  120

/* RGB565 frame from camera */
#define CAM_BUF_SIZE (CAM_W * CAM_H * sizeof(uint16_t))

/* Preview texture size (must be power of 2 for GPU) */
#define PREVIEW_TEX_W 256
#define PREVIEW_TEX_H 128

/* --- State --- */
static bool sInited = false;
static bool sActive = false;
static struct quirc* sQuirc = NULL;

static uint16_t* sCamBuf = NULL; /* RGB565 camera frame */
static uint8_t* sGrayBuf = NULL; /* Downscaled grayscale for quirc + preview */
static s16 sTransferUnit = 0;   /* DMA transfer chunk size */

/* Decoded QR data */
static uint8_t sPayload[4096];
static int sPayloadLen = 0;
static bool sHasResult = false;
static char sStatusMsg[64] = "";
static uint32_t sStatusColor = 0;

/* Frame counter */
static int sFrameCount = 0;

/* Preview texture */
static C3D_Tex sPreviewTex;
static bool sPreviewTexInited = false;

/* Morton LUT for 8x8 tile encoding */
static const uint8_t mortonLut[64] = {
	 0, 1, 4, 5,16,17,20,21,
	 2, 3, 6, 7,18,19,22,23,
	 8, 9,12,13,24,25,28,29,
	10,11,14,15,26,27,30,31,
	32,33,36,37,48,49,52,53,
	34,35,38,39,50,51,54,55,
	40,41,44,45,56,57,60,61,
	42,43,46,47,58,59,62,63,
};

/* Convert a single RGB565 pixel to 8-bit grayscale. */
static inline uint8_t px565ToGray(uint16_t px) {
	uint8_t r = (px >> 11) & 0x1F;
	uint8_t g = (px >> 5) & 0x3F;
	uint8_t b = px & 0x1F;
	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
	return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
}

/* Convert 8-bit grayscale to RGB565 */
static inline uint16_t grayToRgb565(uint8_t g) {
	uint16_t r5 = g >> 3;
	uint16_t g6 = g >> 2;
	uint16_t b5 = g >> 3;
	return (r5 << 11) | (g6 << 5) | b5;
}

int qrscanInit(void) {
	Result rc;

	if (sInited) return 0;

	/* Init quirc */
	sQuirc = quirc_new();
	if (!sQuirc) return -1;
	if (quirc_resize(sQuirc, CAM_W, CAM_H) < 0) {
		quirc_destroy(sQuirc);
		sQuirc = NULL;
		return -1;
	}

	/* Allocate camera buffer (must be in linear memory for DMA) */
	sCamBuf = (uint16_t*)linearAlloc(CAM_BUF_SIZE);
	if (!sCamBuf) {
		quirc_destroy(sQuirc);
		sQuirc = NULL;
		return -1;
	}

	sGrayBuf = (uint8_t*)malloc(CAM_W * CAM_H);
	if (!sGrayBuf) {
		linearFree(sCamBuf);
		quirc_destroy(sQuirc);
		sQuirc = NULL;
		sCamBuf = NULL;
		return -1;
	}

	/* Init camera service */
	rc = camInit();
	if (R_FAILED(rc)) {
		free(sGrayBuf);
		linearFree(sCamBuf);
		quirc_destroy(sQuirc);
		sQuirc = NULL;
		sCamBuf = NULL;
		sGrayBuf = NULL;
		return -1;
	}

	/* Configure outer camera */
	CAMU_SetSize(SELECT_OUT1, SIZE_QVGA, CONTEXT_A);
	CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15);
	CAMU_SetNoiseFilter(SELECT_OUT1, false);
	CAMU_SetAutoExposure(SELECT_OUT1, true);
	CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
	CAMU_SetSharpness(SELECT_OUT1, 7); /* Max sharpness for crisp QR edges */

	/* Transfer settings */
	CAMU_SetTrimming(PORT_CAM1, false);
	{
		u32 maxBytes = 0;
		CAMU_GetMaxBytes(&maxBytes, CAM_W, CAM_H);
		CAMU_SetTransferBytes(PORT_CAM1, maxBytes, CAM_W, CAM_H);
		if (maxBytes > 32767) maxBytes = 32767;
		sTransferUnit = (s16)maxBytes;
	}

	sInited = true;
	sActive = false;
	sHasResult = false;
	sPayloadLen = 0;
	sFrameCount = 0;
	snprintf(sStatusMsg, sizeof(sStatusMsg), "Ready");
	sStatusColor = CLR_GRAY;

	return 0;
}

void qrscanExit(void) {
	if (!sInited) return;

	if (sActive) {
		qrscanStop();
	}

	if (sPreviewTexInited) {
		C3D_TexDelete(&sPreviewTex);
		sPreviewTexInited = false;
	}

	camExit();
	free(sGrayBuf);
	linearFree(sCamBuf);
	quirc_destroy(sQuirc);

	sGrayBuf = NULL;
	sCamBuf = NULL;
	sQuirc = NULL;
	sInited = false;
}

int qrscanIsReady(void) {
	return sInited;
}

void qrscanStart(void) {
	if (!sInited || sActive) return;

	CAMU_Activate(SELECT_OUT1);
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_StartCapture(PORT_CAM1);

	sActive = true;
	sHasResult = false;
	sPayloadLen = 0;
	sFrameCount = 0;

	snprintf(sStatusMsg, sizeof(sStatusMsg), "Scanning...");
	sStatusColor = CLR_YELLOW;
}

void qrscanStop(void) {
	if (!sActive) return;

	CAMU_StopCapture(PORT_CAM1);
	CAMU_Activate(SELECT_NONE);

	sActive = false;
}

int qrscanIsActive(void) {
	return sActive;
}

int qrscanPoll(void) {
	Handle event = 0;
	uint8_t* qBuf;
	int qW, qH;
	int count, i;
	Result rc;

	if (!sActive) return 0;

	/* Request a frame via DMA */
	rc = CAMU_SetReceiving(&event, sCamBuf, PORT_CAM1,
	                        CAM_BUF_SIZE, sTransferUnit);
	if (R_FAILED(rc)) {
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "Recv err: %08lX tu=%d", rc, (int)sTransferUnit);
		sStatusColor = CLR_RED;
		return 0;
	}

	/* Wait for frame */
	rc = svcWaitSynchronization(event, 1000000000ULL); /* 1s timeout */
	svcCloseHandle(event);
	if (rc != 0) {
		return 0;
	}

	sFrameCount++;

	/* Convert full QVGA to grayscale */
	{
		int idx;
		for (idx = 0; idx < CAM_W * CAM_H; idx++)
			sGrayBuf[idx] = px565ToGray(sCamBuf[idx]);
	}

	/* Feed to quirc (let quirc do its own thresholding at full res) */
	qBuf = quirc_begin(sQuirc, &qW, &qH);
	memcpy(qBuf, sGrayBuf, CAM_W * CAM_H);
	quirc_end(sQuirc);
	count = quirc_count(sQuirc);

	for (i = 0; i < count; i++) {
		struct quirc_code code;
		struct quirc_data data;
		quirc_decode_error_t err;

		quirc_extract(sQuirc, i, &code);
		err = quirc_decode(&code, &data);

		if (err != QUIRC_SUCCESS) {
			quirc_flip(&code);
			err = quirc_decode(&code, &data);
		}

		if (err == QUIRC_SUCCESS) {
			sPayloadLen = data.payload_len;
			if (sPayloadLen > (int)sizeof(sPayload))
				sPayloadLen = (int)sizeof(sPayload);
			memcpy(sPayload, data.payload, sPayloadLen);
			sHasResult = true;

			snprintf(sStatusMsg, sizeof(sStatusMsg),
			         "Decoded! %d bytes", sPayloadLen);
			sStatusColor = CLR_GREEN;
			return 1;
		} else {
			snprintf(sStatusMsg, sizeof(sStatusMsg),
			         "f%d found:%d err:%s",
			         sFrameCount, count, quirc_strerror(err));
			sStatusColor = CLR_RED;
		}
	}

	if (count == 0) {
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "f%d caps:%d grids:%d %dx%d",
		         sFrameCount, sQuirc->num_capstones,
		         sQuirc->num_grids, sQuirc->w, sQuirc->h);
		sStatusColor = CLR_YELLOW;
	}

	return 0;
}

int qrscanGetData(uint8_t* buf, int bufSize) {
	int len;
	if (!sHasResult || sPayloadLen == 0) return 0;
	len = sPayloadLen;
	if (len > bufSize) len = bufSize;
	memcpy(buf, sPayload, len);
	return len;
}

/* Write the GRAYSCALE image into the preview texture so we can see
 * exactly what quirc receives. Each gray byte becomes an RGB565 pixel. */
static void updatePreviewTex(void) {
	int x, y;
	uint16_t* texData;

	if (!sPreviewTexInited) {
		C3D_TexInit(&sPreviewTex, PREVIEW_TEX_W, PREVIEW_TEX_H,
		            GPU_RGB565);
		C3D_TexSetFilter(&sPreviewTex, GPU_NEAREST, GPU_NEAREST);
		sPreviewTexInited = true;
		memset(sPreviewTex.data, 0,
		       PREVIEW_TEX_W * PREVIEW_TEX_H * sizeof(uint16_t));
	}

	texData = (uint16_t*)sPreviewTex.data;

	for (y = 0; y < QR_H; y++) {
		for (x = 0; x < QR_W; x++) {
			int bx = x & 7;
			int by = y & 7;
			int tileX = x >> 3;
			int tileY = y >> 3;
			int tileOff = (tileY * (PREVIEW_TEX_W >> 3) + tileX) * 64;
			int pixOff = mortonLut[by * 8 + bx];

			/* Downscale 2x from full QVGA grayscale (Y-flipped for GPU) */
			int srcY = (QR_H - 1 - y) * 2;
			int srcX = x * 2;
			uint8_t g = (uint8_t)(
				((int)sGrayBuf[srcY * CAM_W + srcX] +
				 (int)sGrayBuf[srcY * CAM_W + srcX + 1] +
				 (int)sGrayBuf[(srcY + 1) * CAM_W + srcX] +
				 (int)sGrayBuf[(srcY + 1) * CAM_W + srcX + 1]) >> 2);
			texData[tileOff + pixOff] = grayToRgb565(g);
		}
	}

	GSPGPU_FlushDataCache(sPreviewTex.data,
	                       PREVIEW_TEX_W * PREVIEW_TEX_H * sizeof(uint16_t));
}

void qrscanDraw(struct GUIFont* font, int screenW, int screenH) {
	char buf[64];
	int previewW, previewH, previewX, previewY;

	(void)screenH;

	/* Background */
	drawRect(0, 0, screenW, 240, UI_PANEL);

	/* Header */
	GUIFontPrintf(font, 8, 4, GUI_ALIGN_LEFT, CLR_CYAN, "QR Scanner");

	/* Camera preview — scale to fit width with aspect ratio */
	previewW = screenW - 16;
	previewH = previewW * QR_H / QR_W;
	if (previewH > 160) {
		previewH = 160;
		previewW = previewH * QR_W / QR_H;
	}
	previewX = (screenW - previewW) / 2;
	previewY = 20;

	if ((sActive || sHasResult) && sFrameCount > 0) {
		updatePreviewTex();

		/* Draw the preview texture */
		ctrActivateTexture(&sPreviewTex);
		ctrAddRectEx(0xFFFFFFFF,
		             previewX, previewY, previewW, previewH,
		             0, 0, QR_W, QR_H, 0);
		ctrFlushBatch();
	} else {
		/* Placeholder */
		drawRect(previewX, previewY, previewW, previewH, 0xFF333333);
		GUIFontPrintf(font, screenW / 2, previewY + previewH / 2 - 6,
		              GUI_ALIGN_HCENTER, CLR_GRAY,
		              sActive ? "Waiting for frame..." : "Camera inactive");
	}

	/* Status line */
	GUIFontPrintf(font, 8, previewY + previewH + 8,
	              GUI_ALIGN_LEFT, sStatusColor, "%s", sStatusMsg);

	/* Decoded data preview */
	if (sHasResult && sPayloadLen > 0) {
		int showLen = sPayloadLen;
		if (showLen > 60) showLen = 60;
		memcpy(buf, sPayload, showLen);
		buf[showLen] = '\0';
		GUIFontPrintf(font, 8, previewY + previewH + 22,
		              GUI_ALIGN_LEFT, CLR_WHITE, "%s", buf);
	}

	/* Instructions */
	GUIFontPrintf(font, screenW / 2, 226,
	              GUI_ALIGN_HCENTER, CLR_GRAY,
	              "Point camera at QR code  [B] Exit");
}

const char* qrscanGetStatus(void) {
	return sStatusMsg;
}

/* ===================================================================
 *  QR Payload format (text, all printable ASCII):
 *    PK1:ITEM:IDxQTY,IDxQTY,...   Give items
 *    PK1:HEAL                      Heal party
 *    PK1:MONEY:AMOUNT              Give money
 *    PK1:XP:SLOT:AMOUNT            Give XP to party slot
 * =================================================================== */

/* Gen 3 substructure order for decryption */
static const uint8_t sSubOrder[24][4] = {
	{0,1,2,3}, {0,1,3,2}, {0,2,1,3}, {0,2,3,1},
	{0,3,1,2}, {0,3,2,1}, {1,0,2,3}, {1,0,3,2},
	{1,2,0,3}, {1,2,3,0}, {1,3,0,2}, {1,3,2,0},
	{2,0,1,3}, {2,0,3,1}, {2,1,0,3}, {2,1,3,0},
	{2,3,0,1}, {2,3,1,0}, {3,0,1,2}, {3,0,2,1},
	{3,1,0,2}, {3,1,2,0}, {3,2,0,1}, {3,2,1,0},
};

static int findSubOff(uint32_t pid, int which) {
	int order = pid % 24;
	int pos;
	for (pos = 0; pos < 4; pos++) {
		if (sSubOrder[order][pos] == which)
			return pos * 12;
	}
	return 0;
}

/* Gen 3 growth rate EXP formulas.  Returns total EXP needed for a level. */
/* Growth rate IDs: 0=MedFast, 1=Erratic, 2=Fluctuating, 3=MedSlow, 4=Fast, 5=Slow */
static uint32_t expForLevel(uint8_t rate, int lvl) {
	uint32_t n = (uint32_t)lvl;
	uint32_t n2 = n * n;
	uint32_t n3 = n2 * n;
	if (lvl <= 1) return 0;
	switch (rate) {
	case 0: /* Medium Fast */
		return n3;
	case 1: /* Erratic */
		if (n <= 50)      return (n3 * (100 - n)) / 50;
		else if (n <= 68) return (n3 * (150 - n)) / 100;
		else if (n <= 98) return (n3 * ((1911 - 10 * n) / 3)) / 500;
		else              return (n3 * (160 - n)) / 100;
	case 2: /* Fluctuating */
		if (n <= 15)      return n3 * ((((n + 1) / 3) + 24) / 50);
		else if (n <= 36) return n3 * ((n + 14) / 50);
		else              return n3 * (((n / 2) + 32) / 50);
	case 3: /* Medium Slow */
		return (6 * n3 / 5) - (15 * n2) + (100 * n) - 140;
	case 4: /* Fast */
		return 4 * n3 / 5;
	case 5: /* Slow */
		return 5 * n3 / 4;
	default:
		return n3;
	}
}

/* Find what level a pokemon should be given its total EXP and growth rate. */
static uint8_t levelForExp(uint8_t rate, uint32_t exp) {
	int lvl;
	for (lvl = 2; lvl <= 100; lvl++) {
		if (expForLevel(rate, lvl) > exp)
			return (uint8_t)(lvl - 1);
	}
	return 100;
}

static int giveXP(uint8_t* wram, const uint8_t* rom,
                   const struct RomProfile* prof,
                   int slot, uint32_t amount) {
	uint32_t base = prof->partyData + slot * 100;
	uint8_t* mon = wram + base;
	uint32_t pid, otid, key;
	uint8_t dec[48];
	int growthOff, i;
	uint32_t exp;
	uint16_t checksum;
	uint16_t species;
	uint8_t growthRate, newLevel;

	memcpy(&pid, mon, 4);
	memcpy(&otid, mon + 4, 4);
	key = pid ^ otid;

	/* Decrypt 48 bytes of substructure data */
	{
		const uint32_t* enc = (const uint32_t*)(mon + 0x20);
		uint32_t* d = (uint32_t*)dec;
		for (i = 0; i < 12; i++)
			d[i] = enc[i] ^ key;
	}

	/* Modify EXP in Growth substructure (offset 4 = u32 experience) */
	growthOff = findSubOff(pid, 0);
	memcpy(&species, dec + growthOff, 2);
	memcpy(&exp, dec + growthOff + 4, 4);
	exp += amount;
	memcpy(dec + growthOff + 4, &exp, 4);

	/* Recalculate checksum (sum of 48 bytes as u16 words) */
	checksum = 0;
	for (i = 0; i < 24; i++) {
		uint16_t w;
		memcpy(&w, dec + i * 2, 2);
		checksum += w;
	}
	memcpy(mon + 0x1C, &checksum, 2);

	/* Re-encrypt and write back */
	{
		uint32_t* d = (uint32_t*)dec;
		uint32_t* enc = (uint32_t*)(mon + 0x20);
		for (i = 0; i < 12; i++)
			enc[i] = d[i] ^ key;
	}

	/* Update unencrypted level byte at 0x54 using growth rate from ROM */
	if (prof->baseStats && rom && species < prof->speciesCount) {
		/* gBaseStats: 28 bytes/entry, growth rate at offset 19 */
		growthRate = rom[prof->baseStats + species * 28 + 19];
		newLevel = levelForExp(growthRate, exp);
		mon[0x54] = newLevel;
	}

	return 1;
}

#define POCKET_ITEMS    0
#define POCKET_KEYITEMS 1
#define POCKET_BALLS    2
#define POCKET_TMS      3
#define POCKET_BERRIES  4

static int getItemPocket(uint16_t itemId) {
	if (itemId >= 1 && itemId <= 12) return POCKET_BALLS;
	if (itemId >= 289 && itemId <= 354) return POCKET_TMS;
	if (itemId >= 133 && itemId <= 175) return POCKET_BERRIES;
	return POCKET_ITEMS;
}

static uint32_t getSb1Offset(const uint8_t* iwram, const struct RomProfile* prof) {
	uint32_t sb1Ptr;
	if (prof->sb1PtrIwram == 0) return 0;
	memcpy(&sb1Ptr, iwram + prof->sb1PtrIwram, 4);
	if ((sb1Ptr >> 24) != 0x02) return 0;
	return sb1Ptr & 0x3FFFF;
}

static int addItemToBag(uint8_t* wram, uint32_t sb1Off,
                        const struct RomProfile* prof,
                        uint16_t itemId, uint16_t qty) {
	int pocket = getItemPocket(itemId);
	uint32_t pocketOff = sb1Off + prof->sb1BagPocket[pocket];
	int maxSlots = prof->sb1BagSize[pocket];
	int i, emptySlot = -1;
	uint16_t slotId, slotQty;

	if (prof->sb1BagPocket[pocket] == 0) return 0;

	for (i = 0; i < maxSlots; i++) {
		uint8_t* entry = wram + pocketOff + i * 4;
		memcpy(&slotId, entry, 2);
		memcpy(&slotQty, entry + 2, 2);

		if (slotId == itemId) {
			slotQty += qty;
			if (slotQty > 999) slotQty = 999;
			memcpy(entry + 2, &slotQty, 2);
			return 1;
		}
		if (slotId == 0 && emptySlot < 0) {
			emptySlot = i;
		}
	}

	if (emptySlot >= 0) {
		uint8_t* entry = wram + pocketOff + emptySlot * 4;
		memcpy(entry, &itemId, 2);
		memcpy(entry + 2, &qty, 2);
		return 1;
	}

	return 0;
}

/* Decode a base64 mon blob from the payload string.
 * Advances *pp past the base64 data (and trailing ':' if present).
 * Returns number of bytes decoded, or -1 on error. */
static int parseMonBlob(const char** pp, uint8_t* out, int outMax) {
	char b64[200];
	int b64len = 0;
	const char* p = *pp;
	int decoded;

	while (*p && *p != ':' && b64len < (int)sizeof(b64) - 1)
		b64[b64len++] = *p++;
	b64[b64len] = '\0';
	*pp = p;

	decoded = walkerBase64Decode(b64, out, outMax);
	return decoded;
}

/* Inject a 100-byte mon blob into the next empty party slot.
 * Returns the slot index (0-5), or -1 if party full. */
static int injectMon(uint8_t* wram, const struct RomProfile* prof,
                      const uint8_t* blob) {
	int partyCount = wram[prof->partyCount];
	int targetSlot;
	if (partyCount > 6) partyCount = 6;
	if (partyCount >= 6) return -1;

	targetSlot = partyCount;
	memcpy(wram + prof->partyData + targetSlot * 100, blob, 100);
	wram[prof->partyCount] = (uint8_t)(partyCount + 1);
	return targetSlot;
}

/* Parse PK2:RECV:<base64>:<bonusXP>:<items>[:C<caught_b64>]
 * Inject the walked mon + optional caught mon, apply bonus rewards. */
static int applyWalkerRecv(struct GBA* gba, uint8_t* wram,
                            const struct RomProfile* prof,
                            const char* data) {
	uint8_t monBlob[100];
	int decoded;
	int targetSlot;
	const char* p = data;
	uint32_t bonusXP = 0;
	int caught = 0;

	/* Decode walked mon */
	decoded = parseMonBlob(&p, monBlob, 100);
	if (decoded != 100) {
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "Bad mon data (%d bytes)", decoded);
		sStatusColor = CLR_RED;
		return 0;
	}

	/* Validate: PID should be non-zero */
	if (monBlob[0] == 0 && monBlob[1] == 0 && monBlob[2] == 0 && monBlob[3] == 0) {
		snprintf(sStatusMsg, sizeof(sStatusMsg), "Empty mon data");
		sStatusColor = CLR_RED;
		return 0;
	}

	/* Inject walked mon */
	targetSlot = injectMon(wram, prof, monBlob);
	if (targetSlot < 0) {
		snprintf(sStatusMsg, sizeof(sStatusMsg), "Party full! (6/6)");
		sStatusColor = CLR_RED;
		return 0;
	}

	/* Parse bonus XP (format: :<bonusXP>:...) */
	if (*p == ':') {
		p++;
		while (*p >= '0' && *p <= '9') {
			bonusXP = bonusXP * 10 + (*p - '0');
			p++;
		}
	}

	/* Apply bonus XP */
	if (bonusXP > 0) {
		giveXP(wram, (const uint8_t*)gba->memory.rom, prof, targetSlot, bonusXP);
	}

	/* Parse bonus items (format: :<id>x<qty>,<id>x<qty>,...) */
	if (*p == ':') {
		uint32_t sb1Off = (prof->sb1PtrIwram)
			? getSb1Offset((const uint8_t*)gba->memory.iwram, prof)
			: 0;
		p++;
		if (sb1Off > 0) {
			while (*p && *p != ':') {
				int itemId = 0, qty = 0;
				while (*p >= '0' && *p <= '9') {
					itemId = itemId * 10 + (*p - '0');
					p++;
				}
				if (*p == 'x' || *p == 'X') p++;
				while (*p >= '0' && *p <= '9') {
					qty = qty * 10 + (*p - '0');
					p++;
				}
				if (itemId > 0 && qty > 0) {
					addItemToBag(wram, sb1Off, prof,
					             (uint16_t)itemId, (uint16_t)qty);
				}
				if (*p == ',') p++;
				else break;
			}
		} else {
			/* Skip items section */
			while (*p && *p != ':') p++;
		}
	}

	/* Parse caught mon (format: :C<base64>) */
	if (*p == ':' && *(p + 1) == 'C') {
		uint8_t caughtBlob[100];
		p += 2; /* skip ':C' */
		decoded = parseMonBlob(&p, caughtBlob, 100);
		if (decoded == 100) {
			if (injectMon(wram, prof, caughtBlob) >= 0)
				caught = 1;
		}
	}

	if (caught) {
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "Mon returned +%luXP + catch!",
		         (unsigned long)bonusXP);
	} else {
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "Mon returned! Slot %d +%luXP",
		         targetSlot + 1, (unsigned long)bonusXP);
	}
	sStatusColor = CLR_GREEN;
	return 1;
}

int qrscanApplyReward(struct mGUIRunner* runner) {
#ifdef M_CORE_GBA
	struct GBA* gba;
	uint8_t* wram;
	uint8_t* iwram;
	const struct RomProfile* prof;
	uint32_t sb1Off;
	char payload[512];
	int ok = 0;
	int len;

	if (!sHasResult || sPayloadLen < 4) {
		snprintf(sStatusMsg, sizeof(sStatusMsg), "No payload");
		sStatusColor = CLR_RED;
		return 0;
	}

	/* Copy payload as null-terminated string */
	len = sPayloadLen;
	if (len > (int)sizeof(payload) - 1) len = (int)sizeof(payload) - 1;
	memcpy(payload, sPayload, len);
	payload[len] = '\0';

	/* Validate magic (PK1: for rewards, PK2: for walker) */
	if (memcmp(payload, "PK1:", 4) != 0 && memcmp(payload, "PK2:", 4) != 0) {
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "Bad magic: %.8s", payload);
		sStatusColor = CLR_RED;
		return 0;
	}

	if (!runner || !runner->core || !runner->core->board) return 0;
	if (!romprofileIsSupported()) {
		snprintf(sStatusMsg, sizeof(sStatusMsg), "ROM not supported");
		sStatusColor = CLR_RED;
		return 0;
	}

	gba = (struct GBA*) runner->core->board;
	wram = (uint8_t*) gba->memory.wram;
	iwram = (uint8_t*) gba->memory.iwram;
	prof = romprofileGet();

	/* PK2:RECV — Pokewalker return (no SaveBlock1 needed) */
	if (memcmp(payload + 4, "RECV:", 5) == 0) {
		return applyWalkerRecv(gba, wram, prof, payload + 9);
	}

	sb1Off = getSb1Offset(iwram, prof);
	if (sb1Off == 0) {
		snprintf(sStatusMsg, sizeof(sStatusMsg), "SaveBlock1 not found");
		sStatusColor = CLR_RED;
		return 0;
	}

	if (memcmp(payload + 4, "ITEM:", 5) == 0) {
		/* PK1:ITEM:IDxQTY,IDxQTY,... */
		const char* p = payload + 9;
		int added = 0;

		while (*p) {
			int itemId = 0, qty = 0;
			while (*p >= '0' && *p <= '9') {
				itemId = itemId * 10 + (*p - '0');
				p++;
			}
			if (*p == 'x' || *p == 'X') p++;
			while (*p >= '0' && *p <= '9') {
				qty = qty * 10 + (*p - '0');
				p++;
			}
			if (itemId > 0 && qty > 0) {
				if (addItemToBag(wram, sb1Off, prof,
				                 (uint16_t)itemId, (uint16_t)qty))
					added++;
			}
			if (*p == ',') p++;
			else break;
		}
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "Added %d item%s!", added, added == 1 ? "" : "s");
		sStatusColor = CLR_GREEN;
		ok = 1;

	} else if (memcmp(payload + 4, "HEAL", 4) == 0) {
		int count = wram[prof->partyCount];
		int i;
		if (count > 6) count = 6;
		for (i = 0; i < count; i++) {
			uint32_t base = prof->partyData + i * 100;
			uint16_t maxHP;
			uint32_t zero = 0;
			memcpy(&maxHP, wram + base + 0x58, 2);
			memcpy(wram + base + 0x56, &maxHP, 2);
			memcpy(wram + base + 0x50, &zero, 4);
		}
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "Party healed! (%d mon)", count);
		sStatusColor = CLR_GREEN;
		ok = 1;

	} else if (memcmp(payload + 4, "MONEY:", 6) == 0) {
		uint32_t amount = 0, current;
		const char* p = payload + 10;
		while (*p >= '0' && *p <= '9') {
			amount = amount * 10 + (*p - '0');
			p++;
		}
		memcpy(&current, wram + sb1Off + prof->sb1MoneyOffset, 4);
		current += amount;
		if (current > 999999) current = 999999;
		memcpy(wram + sb1Off + prof->sb1MoneyOffset, &current, 4);
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "+$%lu! (total: $%lu)",
		         (unsigned long)amount, (unsigned long)current);
		sStatusColor = CLR_GREEN;
		ok = 1;

	} else if (memcmp(payload + 4, "XP:", 3) == 0) {
		/* PK1:XP:SLOT:AMOUNT */
		const char* p = payload + 7;
		int slot = 0;
		uint32_t amount = 0;
		int count;

		while (*p >= '0' && *p <= '9') {
			slot = slot * 10 + (*p - '0');
			p++;
		}
		if (*p == ':') p++;
		while (*p >= '0' && *p <= '9') {
			amount = amount * 10 + (*p - '0');
			p++;
		}

		count = wram[prof->partyCount];
		if (count > 6) count = 6;
		if (slot < 0 || slot >= count) {
			snprintf(sStatusMsg, sizeof(sStatusMsg),
			         "Invalid slot %d (party=%d)", slot, count);
			sStatusColor = CLR_RED;
		} else if (amount > 0 && giveXP(wram, (const uint8_t*)gba->memory.rom, prof, slot, amount)) {
			snprintf(sStatusMsg, sizeof(sStatusMsg),
			         "+%lu XP to slot %d!", (unsigned long)amount, slot);
			sStatusColor = CLR_GREEN;
			ok = 1;
		}

	} else {
		snprintf(sStatusMsg, sizeof(sStatusMsg),
		         "Unknown: %.20s", payload + 4);
		sStatusColor = CLR_RED;
	}

	return ok;
#else
	(void)runner;
	return 0;
#endif
}
