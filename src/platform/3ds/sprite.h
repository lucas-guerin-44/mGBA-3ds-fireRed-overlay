/* Pokemon sprite decoder + UI drawing for mGBA 3DS overlay
 * Reads LZ77-compressed 4bpp sprites from FireRed ROM,
 * decodes them into a Citro3D texture, and draws via the GPU batch system.
 * Also provides drawRect for UI panel/frame rendering.
 */

#ifndef SPRITE_H
#define SPRITE_H

#include <stdint.h>

/* Draw a Pokemon front sprite at screen coordinates (x, y).
 * rom:     pointer to GBA ROM data
 * species: species ID (1-411)
 * x, y:    top-left screen position
 * w, h:    draw size in pixels (native sprite is 64x64)
 *
 * Supports a multi-slot cache — up to 8 species can be decoded
 * simultaneously without re-decompression (enough for 6 party + margin).
 */
void drawPokemonSprite(const uint8_t* rom, uint16_t species,
                       int x, int y, int w, int h);

/* Same as drawPokemonSprite but with a grayscale palette (for fainted). */
void drawPokemonSpriteGray(const uint8_t* rom, uint16_t species,
                           int x, int y, int w, int h);

/* Draw a filled rectangle. Color is ABGR (0xAABBGGRR). */
void drawRect(int x, int y, int w, int h, uint32_t abgrColor);

/* Free sprite texture resources */
void spriteFree(void);

#endif
