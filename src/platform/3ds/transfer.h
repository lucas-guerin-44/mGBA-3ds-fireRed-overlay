/* transfer.h — WiFi Pokemon transfer mode.
 * Minimal HTTP server on port 8889:
 *   GET /slot      -> raw 100-byte party slot (encrypted Gen3)
 *   POST /inject   -> receive raw 100-byte slot, inject into party
 *
 * Follows the same "hijack _drawOverlay" pattern as walker.c.
 * The server thread queues incoming data; the main thread applies it.
 */

#ifndef TRANSFER_H
#define TRANSFER_H

#include <mgba-util/gui/font.h>

struct mGUIRunner;

/* Call once at startup. */
void transferInit(void);

/* Begin transfer mode for the given party slot (0-5).
 * Snapshots the slot blob, starts the HTTP server, enters menu state.
 * Returns 1 on success, 0 on failure (no ROM / no party / socket error). */
int transferBegin(struct mGUIRunner* runner, int slot);

/* Poll input and pending network events. Call each frame while active.
 * Returns 0 = still running, 1 = done/success, -1 = cancelled. */
int transferPoll(struct mGUIRunner* runner, uint32_t keys);

/* Draw the transfer mode UI on the bottom screen. */
void transferDraw(struct GUIFont* font, int screenW, int screenH);

/* Stop the HTTP server and clean up sockets. */
void transferEnd(void);

/* Returns 1 if transfer mode is active. */
int transferIsActive(void);

#endif /* TRANSFER_H */