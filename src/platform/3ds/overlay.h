#ifndef OVERLAY_H
#define OVERLAY_H

#include <mgba-util/gui/font.h>

struct mGUIRunner;

void overlayDraw(struct mGUIRunner* runner, struct GUIFont* font, int screenW, int screenH, unsigned keysDown);

#endif
