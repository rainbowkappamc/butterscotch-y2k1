#include "debug_font_renderer.h"

#include "debug_font.h"

#include <libdragon.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUGFONT_LINE_HEIGHT_SCALE 0.80f

/* ═══════════════════════════════════════════════════════════════════════════
 * Create / destroy
 * ═══════════════════════════════════════════════════════════════════════════ */

DebugFontRenderer* DebugFontRenderer_create(void) {
    DebugFontRenderer* r = (DebugFontRenderer*) calloc(1, sizeof(DebugFontRenderer));
    r->align   = DEBUGFONT_ALIGN_LEFT;
    r->spacing = 1.0f;

    /* Convert 8-bit greyscale atlas to IA16 — matches textsystemutil.c approach.
       High byte = intensity (0xFF), low byte = alpha (0xFF glyph, 0x00 background).
       malloc_uncached required: RDP DMA reads directly from this buffer.         */
    const uint8_t* src = (const uint8_t*) debugFontPixels;
    int pixels = DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H;
    r->atlas_buf = (uint16_t*) malloc_uncached(pixels * sizeof(uint16_t));

    for (int i = 0; i < pixels; i++) {
        uint8_t a = src[i] ? 0xFF : 0x00;
        r->atlas_buf[i] = (uint16_t)(0xFF00 | a);
    }

    r->atlas = surface_make_linear(r->atlas_buf, FMT_IA16,
                                   DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H);

    /* Pre-bake per-glyph IA16 surfaces so drawPass never allocs at render time */
    int glyph_count = DEBUGFONT_LAST_CP - DEBUGFONT_FIRST_CP + 1;
    r->glyph_surfs = (surface_t*) calloc(glyph_count, sizeof(surface_t));
    r->glyph_bufs  = (uint16_t**) calloc(glyph_count, sizeof(uint16_t*));

    for (int gi = 0; gi < glyph_count; gi++) {
        const DebugFontGlyphEntry* g = &debugFontGlyphs[gi];
        if (g->w == 0 || g->h == 0) continue;

        uint16_t* gbuf = (uint16_t*) malloc_uncached(g->w * g->h * sizeof(uint16_t));
        for (int row = 0; row < g->h; row++) {
            uint16_t* dst = gbuf + row * g->w;
            uint16_t* atlassrc = r->atlas_buf + (g->y + row) * DEBUGFONT_ATLAS_W + g->x;
            memcpy(dst, atlassrc, g->w * sizeof(uint16_t));
        }
        r->glyph_bufs[gi]  = gbuf;
        r->glyph_surfs[gi] = surface_make(gbuf, FMT_IA16, g->w, g->h, g->w * sizeof(uint16_t));
    }

    debugf("DebugFontRenderer: ready\n");
    return r;
}

void DebugFontRenderer_destroy(DebugFontRenderer* r) {
    if (r->glyph_bufs && r->glyph_surfs) {
        int glyph_count = DEBUGFONT_LAST_CP - DEBUGFONT_FIRST_CP + 1;
        for (int i = 0; i < glyph_count; i++)
            if (r->glyph_bufs[i]) free_uncached(r->glyph_bufs[i]);
        free(r->glyph_bufs);
        free(r->glyph_surfs);
    }
    if (r->atlas_buf) free_uncached(r->atlas_buf);
    free(r);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Glyph helpers  (identical logic to PS2 version)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const DebugFontGlyphEntry* lookupGlyph(uint8_t c) {
    if (c < DEBUGFONT_FIRST_CP || c > DEBUGFONT_LAST_CP) return NULL;
    return &debugFontGlyphs[c - DEBUGFONT_FIRST_CP];
}

static float measureLineWidth(const char* s, int32_t len, float spacing) {
    float w = 0.0f;
    for (int32_t i = 0; i < len; i++) {
        const DebugFontGlyphEntry* g = lookupGlyph((uint8_t) s[i]);
        if (g) w += (float) g->xadvance * spacing;
    }
    return w;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * drawPass
 *
 * Single render pass for an entire (possibly multi-line) string.
 * z is the painter's layer — on N64 draw order equals submission order,
 * so the caller sequences calls by ascending z before invoking printScaled.
 * Caller is responsible for rdpq mode and palette setup before this call.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void drawPass(DebugFontRenderer* r, float x, float y, int z,
                     float scale, color_t color,
                     const char* text, int32_t len) {
    (void) z;

    float   cursorY   = y;
    int32_t lineStart = 0;

    rdpq_set_prim_color(color);

    for (int32_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            int32_t lineLen = i - lineStart;

            float startX = x;
            if (r->align != DEBUGFONT_ALIGN_LEFT) {
                float lineW = measureLineWidth(text + lineStart, lineLen, r->spacing) * scale;
                if      (r->align == DEBUGFONT_ALIGN_CENTER) startX = x - lineW * 0.5f;
                else if (r->align == DEBUGFONT_ALIGN_RIGHT)  startX = x - lineW;
            }

            float pen = startX;
            for (int32_t j = 0; j < lineLen; j++) {
                const DebugFontGlyphEntry* g = lookupGlyph((uint8_t) text[lineStart + j]);
                if (!g) continue;

                if (g->w > 0 && g->h > 0) {
                    float qx0 = pen + (float) g->xoffset * scale;
                    float qy0 = cursorY + (float) g->yoffset * scale;

                    int gi = (uint8_t) text[lineStart + j] - DEBUGFONT_FIRST_CP;
                    rdpq_set_mode_standard();
                    rdpq_set_prim_color(color);
                    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, ZERO, PRIM, ZERO),
                                                       (ZERO, ZERO, ZERO, TEX0)));
                    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                    rdpq_mode_filter(FILTER_BILINEAR);
                    rdpq_blitparms_t bp = { .scale_x = scale, .scale_y = scale };
                    rdpq_tex_blit(&r->glyph_surfs[gi], qx0, qy0, &bp);
                }

                pen += (float) g->xadvance * r->spacing * scale;
            }

            cursorY += (float)(DEBUGFONT_LINE_HEIGHT * scale) * DEBUGFONT_LINE_HEIGHT_SCALE;
            lineStart = i + 1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * printScaled — public entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

void DebugFontRenderer_printScaled(DebugFontRenderer* r,
                                    float x, float y, int z,
                                    float scale, color_t color,
                                    const char* text) {
    if (!text) return;

    int32_t len = (int32_t) strlen(text);

    rdpq_set_mode_standard();
    rdpq_set_prim_color(color);
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, ZERO, PRIM, ZERO),
                                       (ZERO, ZERO, ZERO, TEX0)));
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

    /* Outline pre-pass — 8 offset blits whose edges overlap into a soft halo.
       Disabled when outlineColor.a == 0 (the default).                      */
    if (r->outlineColor.a != 0) {
        static const float OFFSETS[8][2] = {
            {-1,-1}, { 0,-1}, { 1,-1},
            {-1, 0},          { 1, 0},
            {-1, 1}, { 0, 1}, { 1, 1},
        };
        float ro = r->outlineRadius;
        for (int i = 0; i < 8; i++) {
            drawPass(r,
                     x + OFFSETS[i][0] * ro,
                     y + OFFSETS[i][1] * ro,
                     z, scale, r->outlineColor, text, len);
        }
    }

    /* Foreground pass. */
    drawPass(r, x, y, z, scale, color, text, len);

    /* Restore copy mode — matches ova_n64 convention so callers can
       rdpq_tex_blit a surface immediately after without a mode switch.     */
    rdpq_set_mode_copy(false);
}
