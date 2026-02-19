/* Battle log system — poll-diff engine + scrollable log renderer.
 *
 * Each frame we read the active battlers from gBattleMons[] in EWRAM
 * and compare HP/species/status to the previous frame.  Detected changes
 * are pushed into a ring buffer of formatted log lines.
 *
 * The log view replaces the detail panel when toggled via the tab bar.
 */

#include "battle.h"
#include "sprite.h"
#include "romprofile.h"

#include <string.h>
#include <stdio.h>

/* --- Colors (ABGR: 0xAABBGGRR) --- */
#define CLR_WHITE   0xFFFFFFFF
#define CLR_GRAY    0xFFC0C0C0
#define CLR_DARK    0xFF808080
#define CLR_HEADER  0xFFFFFF60
#define CLR_DMG     0xFF5050FF  /* red-ish: damage dealt */
#define CLR_HEAL    0xFF40FF40  /* green: healing */
#define CLR_FAINT   0xFF4040FF /* deep red: KO */
#define CLR_STATUS  0xFFE0A0FF /* purple-ish: status change */
#define CLR_SWITCH  0xFFFFE060 /* cyan: switch-in */
#define CLR_SYSTEM  0xFFA0A0A0 /* gray: system messages */

/* --- UI colors (match overlay.c) --- */
#define UI_PANEL    0xD0231919
#define UI_BORDER   0xFF585050
#define UI_TAB_BG   0xD0382828
#define UI_TAB_HI   0xD0504040

/* --- BattlePokemon struct field offsets (standard Gen 3) --- */
#define BMON_SPECIES   0x00  /* u16 */
#define BMON_MOVES     0x0C  /* u16 × 4 */
#define BMON_HP        0x28  /* u16 */
#define BMON_LEVEL     0x2A  /* u8 */
#define BMON_MAXHP     0x2C  /* u16 */
#define BMON_NICKNAME  0x30  /* 11 bytes Gen3 */
#define BMON_STATUS1   0x4C  /* u32 */
#define BMON_SIZE      0x58  /* 88 bytes per entry */

#define TEXT_DROP      12    /* match overlay.c vertical text offset */

/* --- Log ring buffer --- */
#define LOG_MAX   48
#define LOG_LINE  52

struct LogEntry {
	char     text[LOG_LINE];
	uint32_t color;
};

static struct LogEntry sLog[LOG_MAX];
static int sLogHead  = 0;   /* next write index */
static int sLogCount = 0;   /* total entries (up to LOG_MAX) */
static int sLogScroll = 0;  /* scroll offset from newest */
static int sLogVisible = 0; /* 1 = showing battle log */

/* --- Previous-frame state for diffing --- */
static int      sBattleWasActive = 0;
static uint16_t sPrevSpecies[2]  = {0, 0};  /* [0]=player, [1]=opponent */
static uint16_t sPrevHP[2]       = {0, 0};
static uint32_t sPrevStatus[2]   = {0, 0};
static uint16_t sPendingMove     = 0;       /* move being executed */
static uint8_t  sPendingAttacker = 0xFF;    /* who's using it */

/* ===================================================================
 *  Gen 3 text decoding (duplicated from overlay.c to keep battle.c
 *  self-contained; only used for nicknames in battle log)
 * =================================================================== */
static char decodeChar(uint8_t c) {
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

static void decodeStr(const uint8_t* src, char* dst, int maxLen) {
	int i;
	for (i = 0; i < maxLen; i++) {
		dst[i] = decodeChar(src[i]);
		if (dst[i] == '\0') return;
	}
	dst[maxLen] = '\0';
}

static void readMoveName(const uint8_t* rom, uint16_t moveId, char* buf) {
	const struct RomProfile* p = romprofileGet();
	if (moveId == 0 || moveId >= p->moveCount) {
		buf[0] = '\0';
		return;
	}
	decodeStr(rom + p->moveNames + moveId * p->moveNameLen,
	          buf, p->moveNameLen);
}

/* ===================================================================
 *  Status text helper
 * =================================================================== */
static const char* statusName(uint32_t status) {
	if (status & 0x07)  return "fell asleep";
	if (status & 0x08)  return "was poisoned";
	if (status & 0x10)  return "was burned";
	if (status & 0x20)  return "was frozen";
	if (status & 0x40)  return "was paralyzed";
	if (status & 0x80)  return "was badly poisoned";
	return NULL;
}

/* ===================================================================
 *  Log management
 * =================================================================== */
static void logPush(const char* text, uint32_t color) {
	struct LogEntry* e = &sLog[sLogHead];
	strncpy(e->text, text, LOG_LINE - 1);
	e->text[LOG_LINE - 1] = '\0';
	e->color = color;
	sLogHead = (sLogHead + 1) % LOG_MAX;
	if (sLogCount < LOG_MAX) sLogCount++;
	sLogScroll = 0; /* auto-scroll to newest on new entry */
}

/* Get log entry by index from newest (0 = most recent) */
static struct LogEntry* logGet(int fromNewest) {
	int idx;
	if (fromNewest >= sLogCount) return NULL;
	idx = (sLogHead - 1 - fromNewest + LOG_MAX) % LOG_MAX;
	return &sLog[idx];
}

/* ===================================================================
 *  Read a battle mon's nickname from gBattleMons
 * =================================================================== */
static void readBattleNick(const uint8_t* wram, int battler, char* out) {
	const struct RomProfile* p = romprofileGet();
	uint32_t off = p->battleMons + battler * BMON_SIZE + BMON_NICKNAME;
	decodeStr(wram + off, out, 10);
}

/* ===================================================================
 *  Core poll — called once per frame from overlayDraw
 * =================================================================== */
void battlePoll(const uint8_t* wram, const uint8_t* rom) {
	const struct RomProfile* prof = romprofileGet();
	uint32_t flags;
	int inBattle;
	int b;
	char nick[11];
	char buf[LOG_LINE];

	memcpy(&flags, wram + prof->battleFlags, 4);
	inBattle = (flags != 0) ? 1 : 0;

	/* Battle start / end */
	if (inBattle && !sBattleWasActive) {
		/* Clear log from previous battle */
		sLogHead = 0;
		sLogCount = 0;
		sLogScroll = 0;

		logPush("-- Battle start --", CLR_SYSTEM);
		sPendingMove = 0;
		sPendingAttacker = 0xFF;

		/* Initialize tracking state from current battle mons */
		for (b = 0; b < 2; b++) {
			uint32_t base = prof->battleMons + b * BMON_SIZE;
			memcpy(&sPrevSpecies[b], wram + base + BMON_SPECIES, 2);
			memcpy(&sPrevHP[b],      wram + base + BMON_HP, 2);
			memcpy(&sPrevStatus[b],  wram + base + BMON_STATUS1, 4);
		}
		sLogVisible = 1; /* auto-show on battle start */
	}
	if (!inBattle && sBattleWasActive) {
		logPush("-- Battle end --", CLR_SYSTEM);
		sLogVisible = 0; /* return to party view (tab disappears) */
	}
	sBattleWasActive = inBattle;

	if (!inBattle) return;

	/* Track current move (detect new move being used) */
	{
		uint16_t curMove;
		uint8_t attacker;
		memcpy(&curMove, wram + prof->currentMove, 2);
		attacker = wram[prof->battlerAttacker];

		if (curMove != 0 && curMove != sPendingMove) {
			sPendingMove = curMove;
			sPendingAttacker = attacker;
		}
		if (curMove == 0) {
			sPendingMove = 0;
		}
	}

	/* Diff each battler (0=player, 1=opponent) */
	for (b = 0; b < 2; b++) {
		uint32_t base = prof->battleMons + b * BMON_SIZE;
		uint16_t species, hp, maxhp;
		uint32_t status;
		const char* side = (b == 0) ? "" : "Foe ";

		memcpy(&species, wram + base + BMON_SPECIES, 2);
		memcpy(&hp,      wram + base + BMON_HP, 2);
		memcpy(&maxhp,   wram + base + BMON_MAXHP, 2);
		memcpy(&status,  wram + base + BMON_STATUS1, 4);

		/* Skip if slot is empty */
		if (species == 0) {
			sPrevSpecies[b] = 0;
			sPrevHP[b] = 0;
			sPrevStatus[b] = 0;
			continue;
		}

		/* Switch detection (species changed) */
		if (species != sPrevSpecies[b] && sPrevSpecies[b] != 0) {
			readBattleNick(wram, b, nick);
			snprintf(buf, LOG_LINE, "%s%s was sent out!", side, nick);
			logPush(buf, CLR_SWITCH);
			/* Reset HP tracking for the new mon */
			sPrevHP[b] = hp;
			sPrevStatus[b] = status;
			sPrevSpecies[b] = species;
			continue;
		}

		/* HP change detection */
		if (hp != sPrevHP[b] && sPrevSpecies[b] != 0) {
			int diff = (int)sPrevHP[b] - (int)hp;
			readBattleNick(wram, b, nick);

			if (diff > 0) {
				/* Damage taken — attribute to pending move if available */
				int attacker = (b == 0) ? 1 : 0; /* damage to player = opponent attacked */
				char moveStr[14] = "";

				if (sPendingMove != 0 && sPendingAttacker == attacker) {
					readMoveName(rom, sPendingMove, moveStr);
				}

				if (moveStr[0] != '\0') {
					snprintf(buf, LOG_LINE, "%s%s -%d (%s)",
					         side, nick, diff, moveStr);
				} else {
					snprintf(buf, LOG_LINE, "%s%s -%d HP",
					         side, nick, diff);
				}
				logPush(buf, CLR_DMG);

				/* Clear pending move after attribution */
				if (sPendingMove != 0 && sPendingAttacker == attacker) {
					sPendingMove = 0;
					sPendingAttacker = 0xFF;
				}

				/* KO detection */
				if (hp == 0) {
					snprintf(buf, LOG_LINE, "%s%s fainted!", side, nick);
					logPush(buf, CLR_FAINT);
				}
			} else if (diff < 0) {
				/* Healing */
				snprintf(buf, LOG_LINE, "%s%s +%d HP",
				         side, nick, -diff);
				logPush(buf, CLR_HEAL);
			}
		}

		/* Status change detection */
		if (status != sPrevStatus[b] && sPrevSpecies[b] != 0) {
			/* New status applied (wasn't there before) */
			uint32_t newBits = status & ~sPrevStatus[b];
			if (newBits) {
				const char* sname = statusName(newBits);
				if (sname) {
					readBattleNick(wram, b, nick);
					snprintf(buf, LOG_LINE, "%s%s %s!",
					         side, nick, sname);
					logPush(buf, CLR_STATUS);
				}
			}
			/* Status cured */
			if (sPrevStatus[b] != 0 && status == 0) {
				readBattleNick(wram, b, nick);
				snprintf(buf, LOG_LINE, "%s%s recovered!", side, nick);
				logPush(buf, CLR_HEAL);
			}
		}

		sPrevSpecies[b] = species;
		sPrevHP[b] = hp;
		sPrevStatus[b] = status;
	}
}

/* ===================================================================
 *  Draw the battle log (replaces detail panel)
 * =================================================================== */
int battleDrawLog(struct GUIFont* font, const uint8_t* rom,
                  int panelL, int panelR, int panelTop, int lineH)
{
	int panelW = panelR - panelL;
	int inset = 6;
	int y, i, maxLines, maxScroll;

	(void)rom;

	if (!sLogVisible) return 0;

	/* Background panel */
	drawRect(panelL - 2, panelTop - 2, panelW + 4, 240 - panelTop + 2, UI_BORDER);
	drawRect(panelL, panelTop, panelW, 240 - panelTop, UI_PANEL);

	/* Header */
	y = panelTop + 6 + TEXT_DROP;
	GUIFontPrintf(font, panelL + inset, y, GUI_ALIGN_LEFT,
	              CLR_HEADER, "BATTLE LOG");
	GUIFontPrintf(font, panelR - inset, y, GUI_ALIGN_RIGHT,
	              CLR_DARK, "%d entries", sLogCount);
	y += lineH + 4;

	/* Separator line */
	drawRect(panelL + inset, y - 2, panelW - inset * 2, 1, UI_BORDER);
	y += TEXT_DROP;

	/* Log entries (newest first, scrollable) */
	maxLines = (240 - y - 4) / lineH;
	if (maxLines < 1) maxLines = 1;

	maxScroll = sLogCount - maxLines;
	if (maxScroll < 0) maxScroll = 0;
	if (sLogScroll > maxScroll) sLogScroll = maxScroll;

	for (i = 0; i < maxLines; i++) {
		struct LogEntry* e = logGet(i + sLogScroll);
		if (!e) break;
		GUIFontPrintf(font, panelL + inset + 2, y,
		              GUI_ALIGN_LEFT, e->color, "%s", e->text);
		y += lineH;
	}

	if (sLogCount == 0) {
		GUIFontPrintf(font, panelL + inset + 2, y,
		              GUI_ALIGN_LEFT, CLR_DARK, "(waiting for action...)");
	}

	/* Scroll indicators */
	if (sLogScroll > 0) {
		GUIFontPrintf(font, panelR - inset, panelTop + lineH + 6 + TEXT_DROP,
		              GUI_ALIGN_RIGHT, CLR_DARK, "^ newer");
	}
	if (sLogScroll < maxScroll) {
		GUIFontPrintf(font, panelR - inset, 240 - lineH - 2 + TEXT_DROP,
		              GUI_ALIGN_RIGHT, CLR_DARK, "v older");
	}

	return 1;
}

/* ===================================================================
 *  Draw the tab bar at the very bottom of the screen
 * =================================================================== */
#define TAB_H 16

int battleDrawTab(struct GUIFont* font, int panelL, int panelR,
                  int screenH, int lineH)
{
	int panelW = panelR - panelL;
	int tabY = screenH - TAB_H;
	uint32_t bgClr;
	const char* label;

	(void)lineH;

	if (!sBattleWasActive) return 0;

	bgClr = sLogVisible ? UI_TAB_HI : UI_TAB_BG;
	label = sLogVisible ? "[ PARTY ]" : "[ BATTLE LOG ]";

	drawRect(panelL, tabY, panelW, TAB_H, UI_BORDER);
	drawRect(panelL + 1, tabY + 1, panelW - 2, TAB_H - 2, bgClr);

	GUIFontPrintf(font, panelL + panelW / 2, tabY + TEXT_DROP,
	              GUI_ALIGN_HCENTER, CLR_HEADER, "%s", label);

	return TAB_H;
}

/* ===================================================================
 *  Public state accessors
 * =================================================================== */
int battleIsActive(void) {
	return sBattleWasActive;
}

int battleLogShown(void) {
	return sLogVisible;
}

void battleToggleLog(void) {
	sLogVisible = !sLogVisible;
	sLogScroll = 0;
}

void battleScroll(int delta) {
	sLogScroll += delta;
	if (sLogScroll < 0) sLogScroll = 0;
	/* upper bound clamped in drawLog */
}
