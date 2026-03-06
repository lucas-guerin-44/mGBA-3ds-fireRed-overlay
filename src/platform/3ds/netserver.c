/* netserver.c — Minimal HTTP server for Pokemon party data.
 * Runs on a background thread, serves JSON at GET /party/lead.
 * Thread-safe: main thread snapshots party data under mutex,
 * server thread reads snapshot when serving requests.
 */

#include "netserver.h"
#include "romprofile.h"

#include <mgba/core/core.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#endif
#include "feature/gui/gui-runner.h"
#include <mgba-util/socket.h>
#include <mgba-util/threading.h>

#include <stdio.h>
#include <string.h>

#define NET_PORT_DEFAULT 8888
#define RECV_BUF_SIZE    512
#define SEND_BUF_SIZE    1024
#define MAX_PARTY        6
#define POKEMON_SLOT_SIZE 100

/* ===================================================================
 *  Minimal party slot — mirrors overlay.c PokeSlot (data only)
 * =================================================================== */
struct NetPokeSlot {
	uint16_t species;
	uint16_t moves[4];
	uint8_t  pp[4];
	uint8_t  level;
	uint16_t curHP, maxHP;
	uint16_t atk, def, spe, spa, spd;
	uint32_t status;
	char     nickname[11];
	char     speciesName[12];
};

/* ===================================================================
 *  Shared state (main thread writes, server thread reads)
 * =================================================================== */
static Mutex sMutex;
static volatile bool sRunning;
static Socket sListenSock = INVALID_SOCKET;
static Thread sThread;

static int sPartyCount;
static struct NetPokeSlot sParty[MAX_PARTY];
static bool sHasData; /* true once first snapshot taken */

/* ===================================================================
 *  Gen 3 helpers (duplicated from overlay.c to avoid coupling)
 * =================================================================== */
static const uint8_t sSubstructOrder[24][4] = {
	{0,1,2,3}, {0,1,3,2}, {0,2,1,3}, {0,2,3,1},
	{0,3,1,2}, {0,3,2,1}, {1,0,2,3}, {1,0,3,2},
	{1,2,0,3}, {1,2,3,0}, {1,3,0,2}, {1,3,2,0},
	{2,0,1,3}, {2,0,3,1}, {2,1,0,3}, {2,1,3,0},
	{2,3,0,1}, {2,3,1,0}, {3,0,1,2}, {3,0,2,1},
	{3,1,0,2}, {3,1,2,0}, {3,2,0,1}, {3,2,1,0},
};

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
		if (sSubstructOrder[order][pos] == which)
			return pos * 12;
	}
	return 0;
}

static void readSpeciesName(const uint8_t* rom, uint16_t species, char* buf) {
	const struct RomProfile* p = romprofileGet();
	if (species == 0 || species >= p->speciesCount) {
		strcpy(buf, "???");
		return;
	}
	decodeGen3String(rom + p->speciesNames + species * p->speciesNameLen,
	                 buf, p->speciesNameLen);
}

static int readSlotNet(const uint8_t* wram, const uint8_t* rom,
                       int index, struct NetPokeSlot* out) {
	const struct RomProfile* prof = romprofileGet();
	uint32_t base = prof->partyData + (index * POKEMON_SLOT_SIZE);
	const uint8_t* slot = wram + base;
	uint32_t pid, otid, key;
	uint8_t decrypted[48];
	int growthOff, attackOff;

	pid  = ((const uint32_t*)slot)[0];
	otid = ((const uint32_t*)slot)[1];
	key  = pid ^ otid;

	decryptSubstructs(slot, key, decrypted);

	growthOff = findSubstructOffset(pid, 0);
	memcpy(&out->species, decrypted + growthOff, 2);

	if (out->species == 0 || out->species >= prof->speciesCount)
		return 0;

	readSpeciesName(rom, out->species, out->speciesName);

	attackOff = findSubstructOffset(pid, 1);
	memcpy(&out->moves[0], decrypted + attackOff + 0, 2);
	memcpy(&out->moves[1], decrypted + attackOff + 2, 2);
	memcpy(&out->moves[2], decrypted + attackOff + 4, 2);
	memcpy(&out->moves[3], decrypted + attackOff + 6, 2);
	out->pp[0] = decrypted[attackOff + 8];
	out->pp[1] = decrypted[attackOff + 9];
	out->pp[2] = decrypted[attackOff + 10];
	out->pp[3] = decrypted[attackOff + 11];

	decodeGen3String(slot + 0x08, out->nickname, 10);

	out->level = slot[0x54];
	memcpy(&out->curHP, slot + 0x56, 2);
	memcpy(&out->maxHP, slot + 0x58, 2);
	memcpy(&out->atk,   slot + 0x5A, 2);
	memcpy(&out->def,   slot + 0x5C, 2);
	memcpy(&out->spe,   slot + 0x5E, 2);
	memcpy(&out->spa,   slot + 0x60, 2);
	memcpy(&out->spd,   slot + 0x62, 2);

	memcpy(&out->status, slot + 0x50, 4);

	return 1;
}

/* ===================================================================
 *  Status text helper
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
 *  JSON serialization
 * =================================================================== */
static int serializeSlotJson(const struct NetPokeSlot* s, char* buf, int bufSize) {
	const char* st;
	int n;

	st = statusText(s->status);

	n = snprintf(buf, bufSize,
		"{"
		"\"species\":%u,"
		"\"speciesName\":\"%s\","
		"\"nickname\":\"%s\","
		"\"level\":%u,"
		"\"hp\":%u,"
		"\"maxHp\":%u,"
		"\"atk\":%u,"
		"\"def\":%u,"
		"\"spa\":%u,"
		"\"spd\":%u,"
		"\"spe\":%u,"
		"\"moves\":[%u,%u,%u,%u],"
		"\"pp\":[%u,%u,%u,%u],"
		"\"status\":%s%s%s"
		"}",
		s->species, s->speciesName, s->nickname,
		s->level, s->curHP, s->maxHP,
		s->atk, s->def, s->spa, s->spd, s->spe,
		s->moves[0], s->moves[1], s->moves[2], s->moves[3],
		s->pp[0], s->pp[1], s->pp[2], s->pp[3],
		st ? "\"" : "", st ? st : "null", st ? "\"" : "");

	return n;
}

static int serializePartyJson(char* buf, int bufSize) {
	int n = 0;
	int i;

	n += snprintf(buf + n, bufSize - n, "{\"partyCount\":%d,\"party\":[", sPartyCount);

	for (i = 0; i < sPartyCount && i < MAX_PARTY; i++) {
		if (i > 0) {
			n += snprintf(buf + n, bufSize - n, ",");
		}
		n += serializeSlotJson(&sParty[i], buf + n, bufSize - n);
	}

	n += snprintf(buf + n, bufSize - n, "]}");
	return n;
}

/* ===================================================================
 *  HTTP response helpers
 * =================================================================== */
static void sendHttpResponse(Socket client, int code, const char* status,
                              const char* contentType, const char* body, int bodyLen) {
	char header[256];
	int hLen;

	hLen = snprintf(header, sizeof(header),
		"HTTP/1.0 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Connection: close\r\n"
		"\r\n",
		code, status, contentType, bodyLen);

	SocketSend(client, header, hLen);
	if (bodyLen > 0) {
		SocketSend(client, body, bodyLen);
	}
}

static void handleRequest(Socket client) {
	char recvBuf[RECV_BUF_SIZE];
	char sendBuf[4096];
	ssize_t received;
	int bodyLen;

	received = SocketRecv(client, recvBuf, RECV_BUF_SIZE - 1);
	if (received <= 0) {
		SocketClose(client);
		return;
	}
	recvBuf[received] = '\0';

	MutexLock(&sMutex);

	if (!sHasData || !romprofileIsSupported()) {
		MutexUnlock(&sMutex);
		const char* err = "{\"error\":\"no data\"}";
		sendHttpResponse(client, 503, "Service Unavailable",
		                 "application/json", err, strlen(err));
		SocketClose(client);
		return;
	}

	/* Route: GET /party/lead -> lead Pokemon only */
	if (strstr(recvBuf, "GET /party/lead")) {
		bodyLen = serializeSlotJson(&sParty[0], sendBuf, sizeof(sendBuf));
		MutexUnlock(&sMutex);
		sendHttpResponse(client, 200, "OK", "application/json", sendBuf, bodyLen);
	}
	/* Route: GET /party -> full party */
	else if (strstr(recvBuf, "GET /party")) {
		bodyLen = serializePartyJson(sendBuf, sizeof(sendBuf));
		MutexUnlock(&sMutex);
		sendHttpResponse(client, 200, "OK", "application/json", sendBuf, bodyLen);
	}
	else {
		MutexUnlock(&sMutex);
		const char* err = "{\"error\":\"not found\"}";
		sendHttpResponse(client, 404, "Not Found",
		                 "application/json", err, strlen(err));
	}

	SocketClose(client);
}

/* ===================================================================
 *  Server thread
 * =================================================================== */
static THREAD_ENTRY serverThreadEntry(void* arg) {
	UNUSED(arg);

	while (sRunning) {
		Socket reads[1];
		reads[0] = sListenSock;

		/* Poll with 100ms timeout so we can check sRunning */
		int ready = SocketPoll(1, reads, NULL, NULL, 100);
		if (ready <= 0 || SOCKET_FAILED(reads[0])) {
			continue;
		}

		Socket client = SocketAccept(sListenSock, NULL);
		if (SOCKET_FAILED(client)) {
			continue;
		}

		SocketSetBlocking(client, true);
		SocketSetTCPPush(client, 1);
		handleRequest(client);
	}

	THREAD_EXIT(0);
}

/* ===================================================================
 *  Public API
 * =================================================================== */
int netserverStart(int port) {
	if (sRunning) return 0;

	MutexInit(&sMutex);
	sHasData = false;
	sPartyCount = 0;

	SocketSubsystemInit();

	sListenSock = SocketOpenTCP(port, NULL);
	if (SOCKET_FAILED(sListenSock)) {
		SocketSubsystemDeinit();
		return -1;
	}

	SocketSetBlocking(sListenSock, false);

	if (SocketListen(sListenSock, 1) != 0) {
		SocketClose(sListenSock);
		sListenSock = INVALID_SOCKET;
		SocketSubsystemDeinit();
		return -1;
	}

	sRunning = true;

	if (ThreadCreate(&sThread, serverThreadEntry, NULL) != 0) {
		sRunning = false;
		SocketClose(sListenSock);
		sListenSock = INVALID_SOCKET;
		SocketSubsystemDeinit();
		return -1;
	}

	return 0;
}

void netserverStop(void) {
	if (!sRunning) return;

	sRunning = false;

	if (!SOCKET_FAILED(sListenSock)) {
		SocketClose(sListenSock);
		sListenSock = INVALID_SOCKET;
	}

	ThreadJoin(&sThread);
	SocketSubsystemDeinit();
}

#define UPDATE_INTERVAL_MS 60000

void netserverUpdateParty(struct mGUIRunner* runner) {
#ifdef M_CORE_GBA
	struct GBA* gba;
	const uint8_t* wram;
	const uint8_t* rom;
	const struct RomProfile* prof;
	int count, i;
	static uint64_t sLastUpdate = 0;
	uint64_t now;

	if (!runner || !runner->core || !runner->core->board) return;
	if (!romprofileIsSupported()) return;

	now = osGetTime();
	if (sHasData && (now - sLastUpdate) < UPDATE_INTERVAL_MS) return;
	sLastUpdate = now;

	gba = (struct GBA*) runner->core->board;
	wram = (const uint8_t*) gba->memory.wram;
	rom  = (const uint8_t*) gba->memory.rom;
	prof = romprofileGet();

	count = wram[prof->partyCount];
	if (count > MAX_PARTY) count = MAX_PARTY;

	MutexLock(&sMutex);
	sPartyCount = count;
	for (i = 0; i < count; i++) {
		if (!readSlotNet(wram, rom, i, &sParty[i])) {
			sPartyCount = i;
			break;
		}
	}
	sHasData = (sPartyCount > 0);
	MutexUnlock(&sMutex);
#else
	UNUSED(runner);
#endif
}
