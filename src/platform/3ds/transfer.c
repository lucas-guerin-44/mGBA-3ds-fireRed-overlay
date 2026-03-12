/* transfer.c — WiFi Pokemon transfer mode.
 *
 * HTTP mini-server (port 8889, background thread):
 *   GET  /slot    -> serve raw 100-byte encrypted party slot to PC
 *   POST /inject  -> receive raw 100-byte slot from PC, queue for inject
 *
 * State machine (main thread):
 *   TMODE_MENU  -> show Send / Receive / Cancel choice
 *   TMODE_SEND  -> show IP:port, wait for GET /slot, display result
 *   TMODE_RECV  -> show IP:port, wait for POST /inject, apply to EWRAM
 *
 * The server thread never touches GBA memory directly.
 * All EWRAM writes happen in transferPoll() on the main thread.
 */

#include "transfer.h"
#include "romprofile.h"
#include "ctr-gpu.h"
#include "sprite.h"

#include <mgba/core/core.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#endif
#include "feature/gui/gui-runner.h"
#include <mgba-util/gui/font.h>
#include <mgba-util/socket.h>
#include <mgba-util/threading.h>

#include <3ds.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Constants
 * =================================================================== */
#define TRANSFER_PORT   8889
#define SLOT_SIZE       100
#define RECV_BUF_SIZE   640   /* headers + up to 100 binary bytes */

/* ABGR colors (same palette as overlay.c) */
#define CLR_WHITE   0xFFFFFFFF
#define CLR_GRAY    0xFFC0C0C0
#define CLR_CYAN    0xFFFFFF60
#define CLR_GREEN   0xFF40FF40
#define CLR_RED     0xFF4040FF
#define CLR_DARK    0xFF808080
#define UI_PANEL    0xD0231919
#define UI_BORDER   0xFF585050
#define UI_SEL_BG   0xD0503030
#define UI_ACCENT   0xFF686060

/* ===================================================================
 *  State
 * =================================================================== */
typedef enum {
	TMODE_MENU = 0,
	TMODE_SEND,
	TMODE_RECV,
} TransferState;

static bool          sActive        = false;
static TransferState sState         = TMODE_MENU;
static int           sMenuSel       = 0;    /* 0=Send, 1=Recv, 2=Cancel */
static int           sDoneFrames    = 0;    /* countdown after success */

/* Snapshot of the party slot selected when Transfer Mode was entered */
static int     sSendSlot  = -1;
static uint8_t sMonBlob[SLOT_SIZE];
static char    sSpeciesName[12];
static uint8_t sLevel;

/* HTTP server */
static Socket        sListenSock    = INVALID_SOCKET;
static Thread        sThread;
static volatile bool sThreadRunning = false;
static bool          sSockInited    = false;

/* Flags set by server thread, read/cleared by main thread */
static volatile bool sSentFlag      = false;
static volatile bool sPendingInject = false;
static uint8_t       sPendingBlob[SLOT_SIZE];
static Mutex         sMutex;

/* UI display */
static char     sIpStr[20];
static char     sStatusMsg[64];
static uint32_t sStatusClr;

/* ===================================================================
 *  Gen3 substructure helpers (self-contained, matches overlay.c)
 * =================================================================== */
static const uint8_t sSubOrder[24][4] = {
	{0,1,2,3}, {0,1,3,2}, {0,2,1,3}, {0,2,3,1},
	{0,3,1,2}, {0,3,2,1}, {1,0,2,3}, {1,0,3,2},
	{1,2,0,3}, {1,2,3,0}, {1,3,0,2}, {1,3,2,0},
	{2,0,1,3}, {2,0,3,1}, {2,1,0,3}, {2,1,3,0},
	{2,3,0,1}, {2,3,1,0}, {3,0,1,2}, {3,0,2,1},
	{3,1,0,2}, {3,1,2,0}, {3,2,0,1}, {3,2,1,0},
};

static void decryptSubstructs(const uint8_t* blob, uint32_t key, uint8_t* out) {
	const uint32_t* enc = (const uint32_t*)(blob + 0x20);
	uint32_t* dec = (uint32_t*)out;
	int i;
	for (i = 0; i < 12; i++) dec[i] = enc[i] ^ key;
}

static int growthOffset(uint32_t pid) {
	int order = (int)(pid % 24);
	int pos;
	for (pos = 0; pos < 4; pos++) {
		if (sSubOrder[order][pos] == 0) return pos * 12;
	}
	return 0;
}

static void decodeGen3Str(const uint8_t* src, char* dst, int maxLen) {
	int i;
	for (i = 0; i < maxLen; i++) {
		uint8_t c = src[i];
		if (c == 0xFF) break;
		if (c >= 0xBB && c <= 0xD4) dst[i] = 'A' + (c - 0xBB);
		else if (c >= 0xD5 && c <= 0xEE) dst[i] = 'a' + (c - 0xD5);
		else if (c >= 0xA1 && c <= 0xAA) dst[i] = '0' + (c - 0xA1);
		else dst[i] = '?';
	}
	dst[i] = '\0';
}

/* Extract species name from a raw 100-byte party slot blob + ROM.
 * Safe to call with any blob (validates species range). */
static void speciesNameFromBlob(const uint8_t* blob, const uint8_t* rom,
                                char* nameBuf, int nameBufLen) {
	const struct RomProfile* prof = romprofileGet();
	uint32_t pid, otid, key;
	uint8_t dec[48];
	uint16_t species;
	int off;

	memcpy(&pid,  blob,     4);
	memcpy(&otid, blob + 4, 4);
	key = pid ^ otid;
	decryptSubstructs(blob, key, dec);

	off = growthOffset(pid);
	memcpy(&species, dec + off, 2);

	if (species == 0 || species >= prof->speciesCount || prof->speciesNames == 0) {
		snprintf(nameBuf, nameBufLen, "#%u", species);
		return;
	}
	decodeGen3Str(rom + prof->speciesNames + species * prof->speciesNameLen,
	              nameBuf, nameBufLen - 1);
}

/* ===================================================================
 *  HTTP helpers
 * =================================================================== */
static void sendHttpResponse(Socket client, int code, const char* status,
                              const char* ct, const void* body, int bodyLen) {
	char hdr[256];
	int n = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Connection: close\r\n"
		"\r\n",
		code, status, ct, bodyLen);
	SocketSend(client, hdr, n);
	if (bodyLen > 0) SocketSend(client, body, bodyLen);
}

static void handleRequest(Socket client) {
	char buf[RECV_BUF_SIZE];
	ssize_t n;
	char* sep;
	int bodyReceived;

	n = SocketRecv(client, buf, sizeof(buf) - 1);
	if (n <= 0) { SocketClose(client); return; }
	buf[n] = '\0';

	/* GET /slot — serve the snapshot blob */
	if (memcmp(buf, "GET ", 4) == 0) {
		sendHttpResponse(client, 200, "OK",
		                 "application/octet-stream",
		                 sMonBlob, SLOT_SIZE);
		sSentFlag = true;
		SocketClose(client);
		return;
	}

	/* POST /inject — receive a raw slot blob and queue for inject */
	if (memcmp(buf, "POST ", 5) == 0) {
		uint8_t body[SLOT_SIZE];
		int clen = 0;
		uint32_t pid;
		char* cl;
		ssize_t more;

		/* Parse Content-Length (case-insensitive first char) */
		cl = strstr(buf, "ontent-Length: ");
		if (cl) clen = (int)strtol(cl + 15, NULL, 10);

		if (clen != SLOT_SIZE) {
			sendHttpResponse(client, 400, "Bad Request",
			                 "text/plain", "Expected 100 bytes\n", 19);
			SocketClose(client);
			return;
		}

		/* Locate body after \r\n\r\n */
		sep = strstr(buf, "\r\n\r\n");
		if (!sep) {
			sendHttpResponse(client, 400, "Bad Request",
			                 "text/plain", "Bad request\n", 12);
			SocketClose(client);
			return;
		}
		sep += 4;
		bodyReceived = (int)(n - (sep - buf));
		if (bodyReceived > SLOT_SIZE) bodyReceived = SLOT_SIZE;
		memcpy(body, sep, bodyReceived);

		/* Read remaining bytes if body was split across packets */
		while (bodyReceived < SLOT_SIZE) {
			more = SocketRecv(client, body + bodyReceived,
			                  SLOT_SIZE - bodyReceived);
			if (more <= 0) break;
			bodyReceived += (int)more;
		}

		if (bodyReceived < SLOT_SIZE) {
			sendHttpResponse(client, 400, "Bad Request",
			                 "text/plain", "Incomplete body\n", 16);
			SocketClose(client);
			return;
		}

		/* Basic sanity check: PID must be non-zero */
		memcpy(&pid, body, 4);
		if (pid == 0) {
			sendHttpResponse(client, 400, "Bad Request",
			                 "text/plain", "Empty mon\n", 10);
			SocketClose(client);
			return;
		}

		/* Queue for main-thread inject */
		MutexLock(&sMutex);
		memcpy(sPendingBlob, body, SLOT_SIZE);
		sPendingInject = true;
		MutexUnlock(&sMutex);

		sendHttpResponse(client, 200, "OK", "text/plain", "OK\n", 3);
		SocketClose(client);
		return;
	}

	sendHttpResponse(client, 404, "Not Found", "text/plain", "Not Found\n", 10);
	SocketClose(client);
}

/* ===================================================================
 *  Server thread
 * =================================================================== */
static THREAD_ENTRY serverThreadEntry(void* arg) {
	UNUSED(arg);
	while (sThreadRunning) {
		Socket reads[1];
		int ready;
		reads[0] = sListenSock;
		ready = SocketPoll(1, reads, NULL, NULL, 200);
		if (ready <= 0 || SOCKET_FAILED(reads[0])) continue;
		{
			Socket client = SocketAccept(sListenSock, NULL);
			if (SOCKET_FAILED(client)) continue;
			SocketSetBlocking(client, true);
			SocketSetTCPPush(client, 1);
			handleRequest(client);
		}
	}
	THREAD_EXIT(0);
}

/* ===================================================================
 *  Public API — Init
 * =================================================================== */
void transferInit(void) {
	sActive        = false;
	sThreadRunning = false;
	sSockInited    = false;
	sSentFlag      = false;
	sPendingInject = false;
	sDoneFrames    = 0;
	sIpStr[0]      = '\0';
	MutexInit(&sMutex);
}

/* ===================================================================
 *  Public API — Begin
 * =================================================================== */
int transferBegin(struct mGUIRunner* runner, int slot) {
#ifdef M_CORE_GBA
	struct GBA* gba;
	uint8_t* wram;
	const uint8_t* rom;
	const struct RomProfile* prof;
	int partyCount;
	struct in_addr ipAddr;

	if (!runner || !runner->core || !runner->core->board) return 0;
	if (!romprofileIsSupported()) return 0;

	gba  = (struct GBA*) runner->core->board;
	wram = (uint8_t*) gba->memory.wram;
	rom  = (const uint8_t*) gba->memory.rom;
	prof = romprofileGet();

	partyCount = wram[prof->partyCount];
	if (partyCount > 6) partyCount = 6;
	if (slot < 0 || slot >= partyCount) slot = 0;

	/* Snapshot the selected party slot (needed for SEND; harmless if party empty) */
	if (partyCount > 0) {
		memcpy(sMonBlob, wram + prof->partyData + slot * SLOT_SIZE, SLOT_SIZE);
		sSendSlot = slot;
		sLevel    = sMonBlob[0x54];
		speciesNameFromBlob(sMonBlob, rom, sSpeciesName, sizeof(sSpeciesName));
	} else {
		memset(sMonBlob, 0, SLOT_SIZE);
		sSendSlot = -1;
		sLevel    = 0;
		sSpeciesName[0] = ' ';
	}

	/* Start socket subsystem */
	SocketSubsystemInit();
	sSockInited = true;

	/* Bind and listen */
	sListenSock = SocketOpenTCP(TRANSFER_PORT, NULL);
	if (SOCKET_FAILED(sListenSock)) {
		SocketSubsystemDeinit();
		sSockInited = false;
		snprintf(sStatusMsg, sizeof(sStatusMsg), "Socket error — is WiFi on?");
		sStatusClr = CLR_RED;
		/* Still enter the mode so user can see the error */
	} else {
		SocketSetBlocking(sListenSock, false);
		if (SocketListen(sListenSock, 1) != 0) {
			SocketClose(sListenSock);
			sListenSock = INVALID_SOCKET;
			SocketSubsystemDeinit();
			sSockInited = false;
			snprintf(sStatusMsg, sizeof(sStatusMsg), "Listen failed");
			sStatusClr = CLR_RED;
		} else {
			sThreadRunning = true;
			sSentFlag      = false;
			sPendingInject = false;
			ThreadCreate(&sThread, serverThreadEntry, NULL);

			/* Get local IP for display */
			ipAddr.s_addr = gethostid();
			snprintf(sIpStr, sizeof(sIpStr), "%s", inet_ntoa(ipAddr));
		}
	}

	sState      = TMODE_MENU;
	sMenuSel    = 0;
	sDoneFrames = 0;
	snprintf(sStatusMsg, sizeof(sStatusMsg), "Waiting for connection...");
	sStatusClr  = CLR_GRAY;
	sActive     = true;
	return 1;
#else
	(void)runner; (void)slot;
	return 0;
#endif
}

/* ===================================================================
 *  Public API — Poll (call each frame from main thread)
 * =================================================================== */
int transferPoll(struct mGUIRunner* runner, uint32_t keys) {
	if (!sActive) return -1;

	/* Countdown after a successful transfer — show result briefly */
	if (sDoneFrames > 0) {
		sDoneFrames--;
		if (keys & KEY_B) return 1;   /* early dismiss */
		if (sDoneFrames == 0) return 1;
		return 0;
	}

	/* ── MENU ── */
	if (sState == TMODE_MENU) {
		if (keys & (KEY_CPAD_DOWN | KEY_DOWN)) {
			sMenuSel++;
			if (sMenuSel > 2) sMenuSel = 2;
		}
		if (keys & (KEY_CPAD_UP | KEY_UP)) {
			sMenuSel--;
			if (sMenuSel < 0) sMenuSel = 0;
		}
		if (keys & KEY_A) {
			if (sMenuSel == 0) {
				if (sSendSlot < 0) {
					/* Party is empty — can't send */
					snprintf(sStatusMsg, sizeof(sStatusMsg),
					         "No Pokemon in party!");
					sStatusClr = CLR_RED;
				} else {
					sState = TMODE_SEND;
					snprintf(sStatusMsg, sizeof(sStatusMsg),
					         "Waiting for connection...");
					sStatusClr = CLR_GRAY;
				}
			} else if (sMenuSel == 1) {
				sState = TMODE_RECV;
				snprintf(sStatusMsg, sizeof(sStatusMsg),
				         "Waiting for connection...");
				sStatusClr = CLR_GRAY;
			} else {
				return -1; /* Cancel selected */
			}
		}
		if (keys & KEY_B) return -1;
		return 0;
	}

	/* ── SEND ── */
	if (sState == TMODE_SEND) {
		if (sSentFlag) {
			sSentFlag = false;
			snprintf(sStatusMsg, sizeof(sStatusMsg),
			         "Sent %s (Lv.%u)!", sSpeciesName, sLevel);
			sStatusClr  = CLR_GREEN;
			sDoneFrames = 90;

			/* Remove the slot from the party in WRAM */
			if (runner && runner->core && runner->core->board) {
				struct GBA* gba   = (struct GBA*) runner->core->board;
				uint8_t*    wram  = (uint8_t*) gba->memory.wram;
				const struct RomProfile* prof = romprofileGet();
				if (prof) {
					int count = wram[prof->partyCount];
					if (count > 6) count = 6;
					if (sSendSlot >= 0 && sSendSlot < count) {
						int i;
						for (i = sSendSlot; i < count - 1; i++)
							memcpy(wram + prof->partyData + i * SLOT_SIZE,
							       wram + prof->partyData + (i + 1) * SLOT_SIZE,
							       SLOT_SIZE);
						memset(wram + prof->partyData + (count - 1) * SLOT_SIZE,
						       0, SLOT_SIZE);
						wram[prof->partyCount] = (uint8_t)(count - 1);
					}
				}
			}
		}
		if (keys & KEY_B) return -1;
		return 0;
	}

	/* ── RECV ── */
	if (sState == TMODE_RECV) {
		bool hasInject = false;
		uint8_t blobCopy[SLOT_SIZE];

		MutexLock(&sMutex);
		if (sPendingInject) {
			memcpy(blobCopy, sPendingBlob, SLOT_SIZE);
			sPendingInject = false;
			hasInject = true;
		}
		MutexUnlock(&sMutex);

		if (hasInject) {
#ifdef M_CORE_GBA
			if (runner && runner->core && runner->core->board) {
				struct GBA* gba = (struct GBA*) runner->core->board;
				uint8_t* wram   = (uint8_t*) gba->memory.wram;
				const uint8_t* rom = (const uint8_t*) gba->memory.rom;
				const struct RomProfile* prof = romprofileGet();
				int partyCount = wram[prof->partyCount];
				if (partyCount > 6) partyCount = 6;

				if (partyCount >= 6) {
					snprintf(sStatusMsg, sizeof(sStatusMsg),
					         "Party full! (6/6)");
					sStatusClr = CLR_RED;
					/* Keep listening — user might free a slot via game menu */
				} else {
					int targetSlot = partyCount;
					char name[12];
					uint8_t lvl;
					memcpy(wram + prof->partyData + targetSlot * SLOT_SIZE,
					       blobCopy, SLOT_SIZE);
					wram[prof->partyCount] = (uint8_t)(partyCount + 1);

					speciesNameFromBlob(blobCopy, rom, name, sizeof(name));
					lvl = blobCopy[0x54];
					snprintf(sStatusMsg, sizeof(sStatusMsg),
					         "Received %s (Lv.%u)!", name, lvl);
					sStatusClr  = CLR_GREEN;
					sDoneFrames = 90;
				}
			}
#else
			(void)runner;
#endif
		}

		if (keys & KEY_B) return -1;
		return 0;
	}

	if (keys & KEY_B) return -1;
	return 0;
}

/* ===================================================================
 *  Public API — Draw
 * =================================================================== */
void transferDraw(struct GUIFont* font, int screenW, int screenH) {
	int lineH = GUIFontHeight(font) + 2;
	int y, i;
	int cx = screenW / 2;
	(void)screenH;

	/* Dark background */
	drawRect(0, 0, screenW, 240, UI_PANEL);

	/* ── MENU ── */
	if (sState == TMODE_MENU) {
		static const char* kItems[3] = {
			"Send to PC",
			"Receive from PC",
			"Cancel",
		};

		GUIFontPrintf(font, cx, 22, GUI_ALIGN_HCENTER, CLR_CYAN,
		              "TRANSFER MODE");
		if (sSendSlot >= 0)
			GUIFontPrintf(font, cx, 22 + lineH, GUI_ALIGN_HCENTER, CLR_GRAY,
			              "%s  Lv.%u", sSpeciesName, sLevel);
		else
			GUIFontPrintf(font, cx, 22 + lineH, GUI_ALIGN_HCENTER, CLR_RED,
			              "Party empty");

		drawRect(2, 52, screenW - 4, 1, UI_BORDER);

		y = 60;
		for (i = 0; i < 3; i++) {
			bool disabled = (i == 0 && sSendSlot < 0); /* Send disabled if no party */
			uint32_t bgClr  = (i == sMenuSel && !disabled) ? UI_SEL_BG : UI_PANEL;
			uint32_t txtClr = disabled ? CLR_DARK : (i == sMenuSel) ? CLR_WHITE : CLR_GRAY;
			int rowH = lineH + 6;

			drawRect(8, y - 2, screenW - 16, rowH, bgClr);
			if (i == sMenuSel && !disabled) {
				drawRect(8, y - 2, 3, rowH, UI_ACCENT);
				GUIFontPrintf(font, 18, y + 2, GUI_ALIGN_LEFT, txtClr,
				              "> %s", kItems[i]);
			} else {
				GUIFontPrintf(font, 18, y + 2, GUI_ALIGN_LEFT, txtClr,
				              "  %s", kItems[i]);
			}
			y += rowH + 4;
		}

		drawRect(2, 224, screenW - 4, 1, UI_BORDER);
		GUIFontPrintf(font, cx, 227, GUI_ALIGN_HCENTER, CLR_DARK,
		              "[Up/Down] Select   [A] Confirm   [B] Cancel");
		return;
	}

	/* ── SEND ── */
	if (sState == TMODE_SEND) {
		GUIFontPrintf(font, cx, 22, GUI_ALIGN_HCENTER, CLR_CYAN,
		              "TRANSFER — SEND");
		GUIFontPrintf(font, cx, 22 + lineH, GUI_ALIGN_HCENTER, CLR_WHITE,
		              "%s  Lv.%u", sSpeciesName, sLevel);

		drawRect(2, 50, screenW - 4, 1, UI_BORDER);
		y = 60;

		if (sSockInited && !SOCKET_FAILED(sListenSock)) {
			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_GRAY,
			              "On your PC, run:");
			y += lineH + 2;
			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_WHITE,
			              "GET http://%s:%d/slot", sIpStr, TRANSFER_PORT);
			y += lineH;
			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_GRAY,
			              "Saves a raw .pk3 file (100 bytes)");
		} else {
			GUIFontPrintf(font, cx, y + lineH, GUI_ALIGN_HCENTER, CLR_RED,
			              "No WiFi connection");
		}

		y = 176;
		GUIFontPrintf(font, cx, y, GUI_ALIGN_HCENTER, sStatusClr,
		              "%s", sStatusMsg);

		drawRect(2, 224, screenW - 4, 1, UI_BORDER);
		GUIFontPrintf(font, cx, 227, GUI_ALIGN_HCENTER, CLR_DARK,
		              "[B] Cancel");
		return;
	}

	/* ── RECV ── */
	if (sState == TMODE_RECV) {
		GUIFontPrintf(font, cx, 22, GUI_ALIGN_HCENTER, CLR_CYAN,
		              "TRANSFER — RECEIVE");

		drawRect(2, 38, screenW - 4, 1, UI_BORDER);
		y = 46;

		if (sSockInited && !SOCKET_FAILED(sListenSock)) {
			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_GRAY,
			              "On your PC, run:");
			y += lineH + 2;
			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_WHITE,
			              "POST http://%s:%d/inject", sIpStr, TRANSFER_PORT);
			y += lineH;
			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_GRAY,
			              "Body: raw .pk3 file (100 bytes)");
			y += lineH + 4;

			drawRect(2, y, screenW - 4, 1, UI_BORDER);
			y += 6;

			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_GRAY,
			              "Or with curl:");
			y += lineH + 2;
			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_WHITE,
			              "curl -X POST --data-binary");
			y += lineH;
			GUIFontPrintf(font, 10, y, GUI_ALIGN_LEFT, CLR_WHITE,
			              "  @mon.pk3 http://%s:%d/inject",
			              sIpStr, TRANSFER_PORT);
		} else {
			GUIFontPrintf(font, cx, y + lineH, GUI_ALIGN_HCENTER, CLR_RED,
			              "No WiFi connection");
		}

		y = 204;
		GUIFontPrintf(font, cx, y, GUI_ALIGN_HCENTER, sStatusClr,
		              "%s", sStatusMsg);

		drawRect(2, 224, screenW - 4, 1, UI_BORDER);
		GUIFontPrintf(font, cx, 227, GUI_ALIGN_HCENTER, CLR_DARK,
		              "[B] Cancel");
	}
}

/* ===================================================================
 *  Public API — End
 * =================================================================== */
void transferEnd(void) {
	if (sThreadRunning) {
		sThreadRunning = false;
		if (!SOCKET_FAILED(sListenSock)) {
			SocketClose(sListenSock);
			sListenSock = INVALID_SOCKET;
		}
		ThreadJoin(&sThread);
	} else if (!SOCKET_FAILED(sListenSock)) {
		SocketClose(sListenSock);
		sListenSock = INVALID_SOCKET;
	}
	if (sSockInited) {
		SocketSubsystemDeinit();
		sSockInited = false;
	}
	sActive     = false;
	sDoneFrames = 0;
}

int transferIsActive(void) {
	return sActive ? 1 : 0;
}
