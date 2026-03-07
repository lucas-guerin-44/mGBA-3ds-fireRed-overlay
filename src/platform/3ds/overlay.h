#ifndef OVERLAY_H
#define OVERLAY_H

#include <mgba-util/gui/font.h>

struct mGUIRunner;

void overlayDraw(struct mGUIRunner* runner, struct GUIFont* font, int screenW, int screenH, unsigned keys);

/* Get the currently selected party slot index (0-5). */
int overlayGetSelectedSlot(void);

#endif
