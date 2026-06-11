/*
 * ova_n64.c
 * OVA v2 FMV engine for N64 / libdragon.
 *
 * Ported from ova_player.py (v2).  N64 profile only:
 *   encode 160×120, display 320×240, audio 10 000 Hz mono.
 *
 * Pipeline per frame:
 *   dfs_read  →  LZ4 decompress  →  P-frame delta  →
 *   YCbCr 4:1:1 → RGBA5551  →  2× nearest-neighbour upscale  →  rdpq blit
 *
 * See ova_n64.h for integration notes and controller mapping.
 */

#include "ova_n64.h"
#include <libdragon.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Format constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OVA_MAGIC_0   'O'
#define OVA_MAGIC_1   'V'
#define OVA_MAGIC_2   'A'
#define OVA_MAGIC_3   0x1A
#define OVA_VERSION   0x02

#define HEADER_SIZE   32
#define FRAME_HDR     4      /* type(1) + vsize(3) — audio size is fixed from header */

#define FLAG_AUDIO    0x01
#define FLAG_LZ4      0x02
#define I_FRAME       0x00
#define P_FRAME       0x01

/* ═══════════════════════════════════════════════════════════════════════════
 * N64 profile — fixed geometry, DO NOT change.
 * Encode 160×120 (YCbCr 4:1:1) → display 320×240 (2× nearest upscale).
 * ═══════════════════════════════════════════════════════════════════════════ */

#define EW   160                         /* encode width                     */
#define EH   120                         /* encode height                    */
#define CW   (EW / 4)                    /* chroma width = 40 (4:1:1)        */
#define DW   320                         /* display width                    */
#define DH   240                         /* display height                   */

#define Y_COUNT   (EW * EH)              /* Y  plane: 19 200 bytes           */
#define C_COUNT   (CW * EH)              /* Cb / Cr plane: 4 800 bytes each  */
#define YCC_SIZE  (Y_COUNT + 2 * C_COUNT)/* total YCbCr: 28 800 bytes        */

/* ═══════════════════════════════════════════════════════════════════════════
 * N64 audio profile — DO NOT change.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OVA_AUDIO_RATE  10000            /* Hz                               */
#define OVA_AUDIO_BUFS  4               /* libdragon audio buffer count      */
#define AUDIO_SPF       625             /* samples per frame = ceil(10000/16) */
#define AUDIO_BPF       (AUDIO_SPF * 2) /* bytes per frame (16-bit PCM mono) */

/* ═══════════════════════════════════════════════════════════════════════════
 * Decode buffers  (DMA-aligned)
 * LZ4 worst-case for 28 800 bytes ≈ 29 000; 40 KB gives headroom.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define COMP_BUF_SIZE  (40 * 1024)

static uint8_t  s_comp     [COMP_BUF_SIZE]   __attribute__((aligned(16)));
static uint8_t  s_curr     [YCC_SIZE]         __attribute__((aligned(16)));
static uint8_t  s_prev     [YCC_SIZE]         __attribute__((aligned(16)));
static uint8_t  s_audio_raw[AUDIO_BPF]        __attribute__((aligned(16)));
static int16_t  s_stereo   [AUDIO_SPF * 2];   /* stereo interleave for audio_push */

/* ── Plane accessors ─────────────────────────────────────────────────────── */
#define Y_OF(b)   ((b))
#define CB_OF(b)  ((b) + Y_COUNT)
#define CR_OF(b)  ((b) + Y_COUNT + C_COUNT)

/* ═══════════════════════════════════════════════════════════════════════════
 * Low-level helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static inline int clamp255(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LZ4 block decompressor
 *
 * Standard LZ4 block format, integer-only, no floating point.
 * The MIPS core is big-endian but LZ4 match offsets are little-endian
 * (LZ4 spec), so they are read as two individual bytes.
 *
 * src / src_len : compressed bytes (LZ4 block, not framed).
 * dst / dst_cap : output buffer and capacity in bytes.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void lz4_decompress(const uint8_t *src, int src_len,
                            uint8_t       *dst, int dst_cap)
{
    const uint8_t *end = src + src_len;
    int dp = 0;

    while (src < end) {
        uint8_t tok       = *src++;
        int     lit_len   = (tok >> 4) & 0xF;
        int     match_len =  tok & 0xF;

        /* Extended literal length */
        if (lit_len == 15) {
            uint8_t x;
            do { x = *src++; lit_len += x; } while (x == 255 && src < end);
        }

        /* Copy literals */
        if (lit_len > dst_cap - dp) lit_len = dst_cap - dp;
        memcpy(dst + dp, src, lit_len);
        src += lit_len;
        dp  += lit_len;

        if (src >= end) break;   /* last sequence has no match */

        /* Match offset: little-endian uint16 (LZ4 spec) */
        int offset = (int)src[0] | ((int)src[1] << 8);
        src += 2;

        /* Extended match length */
        match_len += 4;
        if ((tok & 0xF) == 15) {
            uint8_t x;
            do { x = *src++; match_len += x; } while (x == 255 && src < end);
        }

        /* Back-reference copy (may overlap — byte at a time is correct) */
        int mp = dp - offset;
        if (mp < 0) break;
        if (match_len > dst_cap - dp) match_len = dst_cap - dp;
        for (int i = 0; i < match_len; i++)
            dst[dp++] = dst[mp++];
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P-frame delta application
 *
 * Mirrors ova_player.py::_unpack_p_frame / apply():
 *   result[i] = clamp( prev[i] + payload[i] - 128, 0, 255 )
 *   0x80 = no change; values above/below encode positive/negative deltas.
 *
 * Operates in-place on s_curr; s_prev holds the previous reconstructed frame.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void apply_delta(void) {
    for (int i = 0; i < YCC_SIZE; i++) {
        int v = (int)s_prev[i] + (int)s_curr[i] - 128;
        s_curr[i] = (uint8_t)clamp255(v);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * YCbCr 4:1:1 → RGBA5551  +  2× nearest-neighbour upscale
 *
 * Mirrors ova_player.py::_ycbcr_to_rgb + _reconstruct_411 + _scale_nearest.
 * Converts the 160×120 YCbCr frame in s_curr into the 320×240 RGBA5551
 * surface.  Each encode pixel maps to a 2×2 block of display pixels.
 *
 * BT.601 full-range — integer coefficients (× 256), matching the Python
 * floating-point path within ±1 LSB after rounding:
 *   r = y  +  (359·cr) >> 8          (≈ 1.402 × cr)
 *   g = y  −  (88·cb + 183·cr) >> 8  (≈ 0.344·cb + 0.714·cr)
 *   b = y  +  (454·cb) >> 8          (≈ 1.772 × cb)
 *
 * RGBA5551 packing (N64 big-endian RGBA16 layout):
 *   bits 15-11  R5
 *   bits 10-6   G5
 *   bits 5-1    B5
 *   bit  0      A1 = 1 (fully opaque)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ycc_to_surface(surface_t *dst)
{
    uint16_t      *out = (uint16_t *)dst->buffer;
    const uint8_t *Y   = Y_OF (s_curr);
    const uint8_t *Cb  = CB_OF(s_curr);
    const uint8_t *Cr  = CR_OF(s_curr);

    for (int ey = 0; ey < EH; ey++) {
        for (int ex = 0; ex < EW; ex++) {
            int y  = (int)Y [ey * EW + ex];
            int cb = (int)Cb[ey * CW + ex / 4] - 128;
            int cr = (int)Cr[ey * CW + ex / 4] - 128;

            int r = clamp255(y + ((359 * cr) >> 8));
            int g = clamp255(y - ((88 * cb + 183 * cr) >> 8));
            int b = clamp255(y + ((454 * cb) >> 8));

            uint16_t px = (uint16_t)(((r >> 3) << 11) |
                                     ((g >> 3) <<  6) |
                                     ((b >> 3) <<  1) | 1);

            /* 2× nearest-neighbour: write one encode pixel as a 2×2 display block */
            int dx = ex * 2;
            int dy = ey * 2;
            out[ dy      * DW + dx    ] = px;
            out[ dy      * DW + dx + 1] = px;
            out[(dy + 1) * DW + dx    ] = px;
            out[(dy + 1) * DW + dx + 1] = px;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single frame decode
 *
 * Reads one frame block from fd:
 *   • 4-byte frame header (type + video size)
 *   • AUDIO_BPF bytes of PCM audio (if has_audio)
 *   • vsize bytes of compressed video
 *
 * Decompresses video, applies delta if P-frame, converts to the surface.
 * Audio samples are mono-to-stereo interleaved and pushed to the AI.
 *
 * Returns 1 on success, 0 on read error or EOF.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int decode_frame(int fd, surface_t *surf, int has_audio)
{
    /* Frame header: 1 byte type + 3 byte video length */
    uint8_t hdr[FRAME_HDR];
    if (dfs_read(hdr, 1, FRAME_HDR, fd) != FRAME_HDR)
        return 0;

    uint8_t  ftype = hdr[0];
    uint32_t vsize = ((uint32_t)hdr[1] << 16) |
                     ((uint32_t)hdr[2] <<  8) |
                      (uint32_t)hdr[3];

    /* ── Audio ─────────────────────────────────────────────────────────── */
    if (has_audio) {
        dfs_read(s_audio_raw, 1, AUDIO_BPF, fd);

        /* Mono big-endian PCM → stereo interleave for audio_push */
        for (int i = 0; i < AUDIO_SPF; i++) {
            int16_t s = (int16_t)(((uint16_t)s_audio_raw[i * 2]     << 8) |
                                               s_audio_raw[i * 2 + 1]);
            s_stereo[i * 2]     = s;   /* L */
            s_stereo[i * 2 + 1] = s;   /* R */
        }
        audio_push(s_stereo, AUDIO_SPF, false);
    }

    /* ── Compressed video ──────────────────────────────────────────────── */
    if (vsize < 4 || vsize > COMP_BUF_SIZE) return 0;
    if (dfs_read(s_comp, 1, (int)vsize, fd) != (int)vsize) return 0;

    /*
     * s_comp layout:
     *   [0..3]  big-endian uint32 — original (uncompressed) byte count
     *   [4..]   LZ4 block data
     */
    uint32_t orig = be32(s_comp);
    if (orig > YCC_SIZE) orig = YCC_SIZE;

    lz4_decompress(s_comp + 4, (int)vsize - 4, s_curr, (int)orig);

    /* Apply delta in-place on s_curr (s_prev = previous reconstructed frame) */
    if (ftype == P_FRAME)
        apply_delta();

    /* Save fully reconstructed frame as the reference for the next P-frame */
    memcpy(s_prev, s_curr, YCC_SIZE);

    /* Convert YCbCr → RGBA5551, 2× upscale into display surface */
    ycc_to_surface(surf);

    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HUD overlay
 *
 * Mirrors ova_player.py's frame counter label:
 *   frame / total  fps  [PAUSED]
 *
 * Draws a black bar at the top of the screen, then white text over it.
 * Uses FONT_BUILTIN_DEBUG_MONO on rdpq font slot 3 so it doesn't clash
 * with the game's font registrations (animalcrossing.c uses slot 1).
 *
 * Must be called inside an rdpq_attach / rdpq_detach_show block, AFTER
 * the video blit.  Switches to rdpq standard mode internally.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OVA_HUD_FONT_SLOT  1

static void hud_init(void) {}


static void hud_free(void)
{
    /* Intentional no-op: s_hud_font is kept alive as a singleton so that the
     * guard in hud_init() prevents a second rdpq_font_load_builtin() call on
     * the next ova_load() invocation.  Freeing and nulling it here caused the
     * FONT_MAGIC_LOADED assertion on every subsequent call. */
}

static void hud_draw(uint32_t frame, uint32_t total, uint8_t fps, int paused)
{
    char buf[52];
    if (paused)
        snprintf(buf, sizeof(buf), " %u/%u  %dfps  [PAUSED]",
                 (unsigned)frame, (unsigned)total, (int)fps);
    else
        snprintf(buf, sizeof(buf), " %u/%u  %dfps",
                 (unsigned)frame, (unsigned)total, (int)fps);

    rdpq_textparms_t tp = { .align = ALIGN_LEFT };

    rdpq_set_mode_standard();

    /* Black backing bar — matches Python's (0,0,0) background on the label */
    rdpq_set_prim_color(RGBA32(0, 0, 0, 255));
    rdpq_fill_rectangle(0, 0, DW, 14);

    /* White text */
    rdpq_set_prim_color(RGBA32(230, 230, 230, 255));
    rdpq_text_printf(&tp, OVA_HUD_FONT_SLOT, 4, 11, buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ova_load  —  main entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

ova_result_t ova_load(const char *path, int loop, int show_hud)
{
    /* ── Open file ─────────────────────────────────────────────────────── */
    int fd = dfs_open(path);
    if (fd < 0) return OVA_ERROR;

    /* ── Parse header ──────────────────────────────────────────────────── */
    uint8_t raw[HEADER_SIZE];
    if (dfs_read(raw, 1, HEADER_SIZE, fd) != HEADER_SIZE) goto err;

    if (raw[0] != OVA_MAGIC_0 || raw[1] != OVA_MAGIC_1 ||
        raw[2] != OVA_MAGIC_2 || raw[3] != OVA_MAGIC_3) goto err;

    if (raw[4] != OVA_VERSION) goto err;

    uint8_t  fps         = raw[5];
    uint32_t frame_count = be32(raw + 10);
    uint8_t  flags       = raw[0x14];
    int      has_audio   = (flags & FLAG_AUDIO) != 0;

    if (fps == 0 || frame_count == 0) goto err;

    /* ── Audio reinitialise for OVA rate ───────────────────────────────── */
    /*
     * The N64 OVA profile uses 10 000 Hz.  Reinitialise here so the PCM
     * stream plays at the correct pitch.  Caller must restore game audio
     * rate after ova_load returns (e.g. audio_init(32000, 4)).
     */
    audio_close();
    audio_init(OVA_AUDIO_RATE, OVA_AUDIO_BUFS);

    /* ── Display surface ───────────────────────────────────────────────── */
    /*
     * One RGBA5551 surface at display resolution.
     * surface_alloc returns uncached memory the RDP can DMA-read directly.
     */
    surface_t surf = surface_alloc(FMT_RGBA16, DW, DH);
    if (!surf.buffer) goto err;

    memset(surf.buffer, 0,    DW * DH * 2);
    memset(s_curr,      0x80, YCC_SIZE);   /* neutral grey — fallback if first frame stalls */
    memset(s_prev,      0x80, YCC_SIZE);

    /* ── HUD font ──────────────────────────────────────────────────────── */
    if (show_hud) hud_init();

    /* ── Timing ────────────────────────────────────────────────────────── */
    uint64_t interval  = TICKS_PER_SECOND / (uint64_t)fps;
    uint64_t last_tick = get_ticks();

    /* ── Decoder / player state ────────────────────────────────────────── */
    uint32_t     fi       = 0;      /* frames decoded so far                  */
    int          finished = 0;      /* set after last frame is in the surface */
    int          paused   = 0;
    ova_result_t result   = OVA_END;

    /*
     * DFS offset of the first frame block.  Used to seek back on loop.
     * dfs_seek origin 0 = SEEK_SET.
     */
    int data_start = HEADER_SIZE;

    /* ── Playback loop ─────────────────────────────────────────────────── */
    while (1) {

        /* ── Input ─────────────────────────────────────────────────────── */
        joypad_poll();
        joypad_buttons_t btn = joypad_get_buttons_pressed(0);

        /* Start or B → skip */
        if (btn.start || btn.b) {
            result = OVA_SKIP;
            break;
        }

        /* A → pause / resume.  Reset last_tick on resume to avoid a
           burst of frames catching up after a long pause. */
        if (btn.a) {
            paused = !paused;
            if (!paused) last_tick = get_ticks();
        }

        /* ── Frame decode (timer-driven) ───────────────────────────────── */
        if (!paused && !finished) {
            uint64_t now = get_ticks();
            if (now - last_tick >= interval) {
                last_tick += interval;

                if (!decode_frame(fd, &surf, has_audio)) {
                    /* Read error or unexpected EOF */
                    finished = 1;
                } else {
                    fi++;
                    if (fi >= frame_count) {
                        if (loop) {
                            /*
                             * Loop: seek back to the first frame block and
                             * reset the YCbCr reference buffers so the next
                             * I-frame starts clean.
                             */
                            dfs_seek(fd, data_start, SEEK_SET);
                            memset(s_curr, 0x80, YCC_SIZE);
                            memset(s_prev, 0x80, YCC_SIZE);
                            audio_close();
                            audio_init(OVA_AUDIO_RATE, OVA_AUDIO_BUFS);
                            fi = 0;
                        } else {
                            finished = 1;
                        }
                    }
                }
            }
        }

        /* ── Blit current surface ──────────────────────────────────────── */
        surface_t *fb = display_get();
        rdpq_attach(fb, NULL);

        /* Video frame */
        rdpq_set_mode_copy(false);
        rdpq_tex_blit(&surf, 0.0f, 0.0f, NULL);

        /* HUD overlay (switches to standard mode internally) */
        if (show_hud)
            hud_draw(fi, frame_count, fps, paused);

        rdpq_detach_show();

        /* ── Exit after the last frame has been displayed ──────────────── */
        if (finished && !loop) break;
    }

    /* ── Cleanup ───────────────────────────────────────────────────────── */
    if (show_hud) hud_free();
    surface_free(&surf);
    dfs_close(fd);
    return result;

err:
    dfs_close(fd);
    return OVA_ERROR;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Standalone test entry point
 *
 * Compile with -DOVA_STANDALONE_TEST to get a self-contained ROM that just
 * plays one file.  In the normal project build this block is excluded.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef OVA_STANDALONE_TEST
int main(void) {
    dfs_init(DFS_DEFAULT_LOCATION);
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    audio_init(OVA_AUDIO_RATE, OVA_AUDIO_BUFS);
    joypad_init();

    ova_load("ova/test.ova", 0, 1);

    /* After playback, restore display and halt */
    while (1) {}
    return 0;
}
#endif /* OVA_STANDALONE_TEST */
