/* walker.h — Pokewalker-style mon send/receive via QR codes.
 * Send: serialize a party mon into a QR code displayed on screen.
 * Receive: handled by qrscan.c (PK2:RECV payload).
 */

#ifndef WALKER_H
#define WALKER_H

#include <mgba-util/gui/font.h>

struct mGUIRunner;

/* Initialize the walker system (call once at startup). */
void walkerInit(void);

/* Start the "send mon" flow for the given party slot (0-5).
 * Reads the mon from EWRAM, generates a QR code, enters display mode. */
int walkerStartSend(struct mGUIRunner* runner, int slot);

/* Returns true if the walker is currently displaying a QR code. */
int walkerIsActive(void);

/* Draw the QR display screen (call each frame while active). */
void walkerDraw(struct GUIFont* font, int screenW, int screenH);

/* Handle input while QR is displayed.
 * Returns 1 if user confirmed (A), -1 if cancelled (B), 0 otherwise. */
int walkerPoll(uint32_t keysDown);

/* Confirm the send: zero the party slot, compact the party.
 * Call after walkerPoll returns 1. */
int walkerConfirmSend(struct mGUIRunner* runner);

/* Cancel/cleanup without modifying the party. */
void walkerCancel(void);

/* Base64 decode utility (used by qrscan.c for PK2:RECV payloads).
 * Returns number of bytes decoded, or -1 on error. */
int walkerBase64Decode(const char* src, uint8_t* dst, int dstMax);

#endif
