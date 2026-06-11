#pragma once

#include <libdragon.h>
#include <stdint.h>

#define DEBUGFONT_ALIGN_LEFT   0
#define DEBUGFONT_ALIGN_CENTER 1
#define DEBUGFONT_ALIGN_RIGHT  2

typedef struct {
    surface_t   atlas;
    uint16_t*   atlas_buf;
    surface_t*  glyph_surfs;
    uint16_t**  glyph_bufs;
    uint8_t     align;
    float       spacing;
    color_t     outlineColor;
    float       outlineRadius;
} DebugFontRenderer;

/* Initialises the atlas surface from the pre-baked .rodata pixel data.
   Call once at boot, before any text drawing. */
DebugFontRenderer* DebugFontRenderer_create(void);
void               DebugFontRenderer_destroy(DebugFontRenderer* r);

/* Draw text at (x, y) on painter's layer z.
   Draw order equals submission order on N64 — caller sequences calls by z.
   Switches rdpq to standard mode internally; restores copy mode on return. */
void               DebugFontRenderer_printScaled(DebugFontRenderer* r,
                                                  float x, float y, int z,
                                                  float scale, color_t color,
                                                  const char* text);
