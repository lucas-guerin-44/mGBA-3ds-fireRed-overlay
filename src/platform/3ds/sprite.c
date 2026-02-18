/* Pokemon sprite decoder for mGBA 3DS overlay
 *
 * Pipeline:  ROM pointer table -> LZ77 decompress -> 4bpp tile decode
 *            -> palette apply (RGB555->GPU_RGBA8) -> Morton-order into C3D_Tex
 *            -> draw via ctrActivateTexture + ctrAddRectEx
 *
 * FireRed US v1.0 only.
 * Multi-slot cache: up to 8 species decoded simultaneously (enough for
 * a 6-member party sidebar + detail view without per-frame re-decoding).
 */

#include "sprite.h"
#include "romprofile.h"
#include "ctr-gpu.h"

#include <string.h>
#include <3ds.h>
#include <citro3d.h>

#define SPRITE_DIM     64                 /* 64x64 pixels */
#define TILE_SIZE      8
#define TILES_PER_ROW  (SPRITE_DIM / TILE_SIZE) /* 8 */
#define BPP4_TILE_BYTES 32                /* 8*8 / 2 */
#define MAX_DECOMP     4096               /* 64*64*0.5 = 2048, margin for safety */

/* --- Multi-slot sprite texture cache --- */
#define SPRITE_CACHE_SIZE 8

static C3D_Tex  sSpriteTex[SPRITE_CACHE_SIZE];
static uint16_t sCachedSpecies[SPRITE_CACHE_SIZE];
static int      sTexInited[SPRITE_CACHE_SIZE];
static int      sNextEvict = 0;

/* --- Solid-color rectangle support (8x8 white texture) --- */
static C3D_Tex  sWhiteTex;
static int      sWhiteInited = 0;

static void initWhiteTex(void) {
	if (sWhiteInited) return;
	C3D_TexInit(&sWhiteTex, 8, 8, GPU_RGBA8);
	C3D_TexSetFilter(&sWhiteTex, GPU_NEAREST, GPU_NEAREST);
	memset(sWhiteTex.data, 0xFF, 8 * 8 * 4);
	GSPGPU_FlushDataCache(sWhiteTex.data, 8 * 8 * 4);
	sWhiteInited = 1;
}

void drawRect(int x, int y, int w, int h, uint32_t abgrColor) {
	initWhiteTex();
	ctrActivateTexture(&sWhiteTex);
	ctrAddRectEx(abgrColor, (s16)x, (s16)y, (s16)w, (s16)h,
	             0, 0, 1, 1, 0);
}

/* ===================================================================
 *  LZ77 decompressor (GBA BIOS type 0x10)
 *
 *  Header (4 bytes): byte 0 = 0x10, bytes 1-3 = decompressed size (LE)
 *  Stream: flag byte, then 8 chunks MSB-first.
 *    bit=1: compressed — 2 bytes: length(4) + offset(12), copies length+3
 *    bit=0: literal — 1 byte copied verbatim
 * =================================================================== */
static int decompLZ77(const uint8_t* src, uint8_t* dst, int maxOut) {
	uint32_t header;
	int size, si, di, bit;

	memcpy(&header, src, 4);
	if ((header & 0xFF) != 0x10) return 0;

	size = (header >> 8) & 0x00FFFFFF;
	if (size > maxOut) return 0;

	si = 4;
	di = 0;
	while (di < size) {
		uint8_t flags = src[si++];
		for (bit = 7; bit >= 0 && di < size; bit--) {
			if (flags & (1 << bit)) {
				uint8_t b1 = src[si++];
				uint8_t b2 = src[si++];
				int len = ((b1 >> 4) & 0xF) + 3;
				int off = (((b1 & 0xF) << 8) | b2) + 1;
				int i;
				for (i = 0; i < len && di < size; i++) {
					dst[di] = dst[di - off];
					di++;
				}
			} else {
				dst[di++] = src[si++];
			}
		}
	}
	return size;
}

/* ===================================================================
 *  Morton (Z-order) index for pixel within an 8x8 tile
 *  Interleaves x and y bits: result = y2 x2 y1 x1 y0 x0
 * =================================================================== */
static inline uint32_t mortonIdx(int x, int y) {
	return (uint32_t)(
		 (x & 1)       | ((y & 1) << 1) |
		((x & 2) << 1) | ((y & 2) << 2) |
		((x & 4) << 2) | ((y & 4) << 3)
	);
}

/* Pixel offset in Morton-ordered texture data (no Y flip — GPU UV mapping
 * handles the coordinate system difference naturally) */
static inline uint32_t texOffset(int x, int y, int texW) {
	return ((y >> 3) * (texW >> 3) + (x >> 3)) * 64 + mortonIdx(x & 7, y & 7);
}

/* ===================================================================
 *  Decode sprite from ROM into a specific cache slot
 * =================================================================== */
static int decodeSpriteSlot(const uint8_t* rom, uint16_t species, int slot,
                            int grayscale) {
	uint32_t sprPtr, palPtr;
	uint32_t sprOff, palOff;
	uint8_t decomp[MAX_DECOMP];
	uint8_t palRaw[64];
	uint32_t palette[16];
	uint32_t* texData;
	int decompSize, palSize, i;
	int tx, ty, py, px;
	const uint8_t* palSrc;

	const struct RomProfile* prof = romprofileGet();

	/* Read sprite data pointer (8-byte table entries) */
	memcpy(&sprPtr, rom + prof->spriteTable + species * 8, 4);
	if ((sprPtr >> 24) != 0x08) return 0;
	sprOff = sprPtr & 0x01FFFFFF;

	/* Read palette pointer (8-byte table entries) */
	memcpy(&palPtr, rom + prof->paletteTable + species * 8, 4);
	if ((palPtr >> 24) != 0x08) return 0;
	palOff = palPtr & 0x01FFFFFF;

	/* LZ77 decompress the sprite tiles */
	decompSize = decompLZ77(rom + sprOff, decomp, MAX_DECOMP);
	if (decompSize == 0) return 0;

	/* Decompress palette (Gen 3 palettes are LZ77-compressed too).
	 * Decompressed = 32 bytes (16 colors × 2 bytes RGB555).
	 * Fall back to raw read if not LZ77-compressed. */
	palSize = decompLZ77(rom + palOff, palRaw, 64);
	if (palSize >= 32) {
		palSrc = palRaw;
	} else {
		palSrc = rom + palOff;
	}

	/* Build palette: RGB555 -> GPU_RGBA8
	 * 3DS GPU_RGBA8 byte order in memory: A B G R (low→high).
	 * As uint32_t on LE ARM: (R<<24) | (G<<16) | (B<<8) | A */
	palette[0] = 0x00000000;
	for (i = 1; i < 16; i++) {
		uint16_t c;
		uint8_t r, g, b;
		memcpy(&c, palSrc + i * 2, 2);
		r = ((c >>  0) & 0x1F) << 3;
		g = ((c >>  5) & 0x1F) << 3;
		b = ((c >> 10) & 0x1F) << 3;
		if (grayscale) {
			uint8_t lum = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
			r = g = b = lum;
		}
		palette[i] = ((uint32_t)r << 24) | ((uint32_t)g << 16)
		           | ((uint32_t)b <<  8) | 0xFF;
	}

	/* Init texture for this slot (64x64, RGBA8) */
	if (!sTexInited[slot]) {
		C3D_TexInit(&sSpriteTex[slot], SPRITE_DIM, SPRITE_DIM, GPU_RGBA8);
		C3D_TexSetFilter(&sSpriteTex[slot], GPU_NEAREST, GPU_NEAREST);
		sTexInited[slot] = 1;
	}

	/* Clear texture to transparent */
	texData = (uint32_t*)sSpriteTex[slot].data;
	memset(texData, 0, SPRITE_DIM * SPRITE_DIM * 4);

	/* Convert 4bpp GBA tiles -> palette-applied GPU_RGBA8 pixels in Morton order.
	 *
	 * GBA 4bpp layout:  8x8 tiles, each tile = 32 bytes.
	 * Each byte holds two pixels: low nybble = left, high nybble = right.
	 * Tiles are ordered left-to-right, top-to-bottom in the sprite. */
	for (ty = 0; ty < TILES_PER_ROW; ty++) {
		for (tx = 0; tx < TILES_PER_ROW; tx++) {
			int tileIdx = ty * TILES_PER_ROW + tx;
			const uint8_t* tile = decomp + tileIdx * BPP4_TILE_BYTES;

			for (py = 0; py < TILE_SIZE; py++) {
				for (px = 0; px < TILE_SIZE; px += 2) {
					uint8_t byte = tile[py * (TILE_SIZE / 2) + px / 2];
					uint8_t lo = byte & 0x0F;
					uint8_t hi = (byte >> 4) & 0x0F;

					int sx = tx * TILE_SIZE + px;
					int sy = ty * TILE_SIZE + py;

					texData[texOffset(sx,     sy, SPRITE_DIM)] = palette[lo];
					texData[texOffset(sx + 1, sy, SPRITE_DIM)] = palette[hi];
				}
			}
		}
	}

	/* Flush CPU data cache so the GPU sees updated texture */
	GSPGPU_FlushDataCache(texData, SPRITE_DIM * SPRITE_DIM * 4);

	return 1;
}

/* ===================================================================
 *  Cache lookup: find existing slot or decode into a new one
 * =================================================================== */
static C3D_Tex* findOrDecode(const uint8_t* rom, uint16_t species,
                             int grayscale) {
	/* Encode grayscale flag into cache key so both versions can coexist */
	uint16_t cacheKey = species | (grayscale ? 0x8000 : 0);
	int i;

	/* Check cache for existing decode */
	for (i = 0; i < SPRITE_CACHE_SIZE; i++) {
		if (sTexInited[i] && sCachedSpecies[i] == cacheKey)
			return &sSpriteTex[i];
	}

	/* Find first empty slot */
	for (i = 0; i < SPRITE_CACHE_SIZE; i++) {
		if (!sTexInited[i]) {
			if (decodeSpriteSlot(rom, species, i, grayscale)) {
				sCachedSpecies[i] = cacheKey;
				return &sSpriteTex[i];
			}
			return NULL;
		}
	}

	/* All slots full — evict round-robin */
	i = sNextEvict;
	sNextEvict = (sNextEvict + 1) % SPRITE_CACHE_SIZE;
	if (decodeSpriteSlot(rom, species, i, grayscale)) {
		sCachedSpecies[i] = cacheKey;
		return &sSpriteTex[i];
	}
	return NULL;
}

/* ===================================================================
 *  Public: draw sprite at screen position with arbitrary size
 * =================================================================== */
static void drawPokemonSpriteInternal(const uint8_t* rom, uint16_t species,
                                      int x, int y, int w, int h,
                                      int grayscale) {
	C3D_Tex* tex;

	if (species == 0 || species >= romprofileGet()->speciesCount) return;
	if (w <= 0 || h <= 0) return;

	tex = findOrDecode(rom, species, grayscale);
	if (!tex) return;

	/* Bind sprite texture and emit one textured quad.
	 * The batch system handles texture switches and flushing. */
	ctrActivateTexture(tex);
	ctrAddRectEx(0xFFFFFFFF,                        /* color: white (no tint) */
	             (s16)x, (s16)(y + h),               /* screen pos (bottom-left, negative h draws upward) */
	             (s16)w, (s16)(-h),                   /* screen size (negative h = flip Y for correct orientation) */
	             0, 0,                                /* UV origin */
	             SPRITE_DIM, SPRITE_DIM,              /* UV size (full texture) */
	             0);                                  /* no rotation */
}

void drawPokemonSprite(const uint8_t* rom, uint16_t species,
                       int x, int y, int w, int h) {
	drawPokemonSpriteInternal(rom, species, x, y, w, h, 0);
}

void drawPokemonSpriteGray(const uint8_t* rom, uint16_t species,
                           int x, int y, int w, int h) {
	drawPokemonSpriteInternal(rom, species, x, y, w, h, 1);
}

void spriteFree(void) {
	int i;
	for (i = 0; i < SPRITE_CACHE_SIZE; i++) {
		if (sTexInited[i]) {
			C3D_TexDelete(&sSpriteTex[i]);
			sTexInited[i] = 0;
			sCachedSpecies[i] = 0;
		}
	}
	sNextEvict = 0;
	if (sWhiteInited) {
		C3D_TexDelete(&sWhiteTex);
		sWhiteInited = 0;
	}
}
