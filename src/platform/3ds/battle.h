/* Battle log system for the Pokemon overlay.
 * Polls GBA EWRAM each frame to detect battle events (damage, KOs,
 * switches, status changes) via frame-to-frame diffing of gBattleMons.
 * Renders a scrollable combat log that replaces the detail panel.
 */

#ifndef BATTLE_H
#define BATTLE_H

#include <mgba-util/gui/font.h>
#include <stdint.h>

/* Call once per frame to poll battle state and detect events.
 * wram/rom come from gba->memory. */
void battlePoll(const uint8_t* wram, const uint8_t* rom);

/* Draw the battle log panel (replaces detail view when active).
 * Returns 1 if the log was drawn, 0 if not visible. */
int battleDrawLog(struct GUIFont* font, const uint8_t* rom,
                  int panelL, int panelR, int panelTop, int lineH);

/* Draw the battle tab bar at the very bottom of the screen.
 * Only draws when in battle. Returns tab height if drawn, 0 otherwise. */
int battleDrawTab(struct GUIFont* font, int panelL, int panelR,
                  int screenH, int lineH);

/* Returns 1 if currently in a battle */
int battleIsActive(void);

/* Returns 1 if the battle log view is currently shown */
int battleLogShown(void);

/* Toggle the battle log view on/off */
void battleToggleLog(void);

/* Scroll the battle log (positive = down/older, negative = up/newer) */
void battleScroll(int delta);

#endif
