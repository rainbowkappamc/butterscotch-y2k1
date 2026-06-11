/*
 * ova_n64.h
 * OVA v2 FMV engine for N64 / libdragon.
 *
 * Drop ova_n64.c + ova_n64.h into your project and add ova_n64.c to the
 * src list in your Makefile.  Then call ova_load() from any game state.
 *
 * ── Quick integration example ─────────────────────────────────────────────
 *
 *   #include "engine/ova_n64.h"
 *
 *   // Inside a state handler, e.g. DBG_SPLASH in animalcrossing.c:
 *   ova_result_t r = ova_load("ova/opening.ova", 0, 1);
 *   audio_init(32000, 4);   // restore game audio rate
 *   if (r == OVA_SKIP) { ... }
 *
 * ── Audio note ────────────────────────────────────────────────────────────
 *
 *   ova_load() reinitialises audio to the OVA file's embedded rate
 *   (N64 profile: 10 000 Hz) before playback begins.
 *   After it returns you MUST restore your game's audio rate, e.g.:
 *
 *       audio_init(32000, 4);
 *
 *   If the file has no audio track (has_audio flag clear) the audio
 *   subsystem is left untouched.
 *
 * ── OVA v2 format recap (big-endian multi-byte fields) ───────────────────
 *
 *   File header  (32 bytes)
 *     0x00  4   Magic  'O','V','A',0x1A
 *     0x04  1   Version  (0x02)
 *     0x05  1   FPS
 *     0x06  2   Display width   (320)
 *     0x08  2   Display height  (240)
 *     0x0A  4   Frame count
 *     0x0E  2   Audio rate      (10 000 — N64 profile)
 *     0x10  1   Audio channels  (1 = mono)
 *     0x11  2   Audio samples per frame  (625)
 *     0x13  1   Color format    (0x01 = YCbCr 4:1:1)
 *     0x14  1   Flags           (bit0 = has_audio, bit1 = lz4)
 *     0x15  2   Encode width    (160)
 *     0x17  2   Encode height   (120)
 *     0x19  1   GOP size
 *     0x1A  1   Quant Y
 *     0x1B  1   Quant C
 *     0x1C  4   Reserved
 *
 *   Frame block  (per frame, repeated frame_count times)
 *     0x00  1   Frame type   (0x00 = I-frame, 0x01 = P-frame)
 *     0x01  3   Video byte length  (uint24, big-endian)
 *     [optional audio block — size fixed at audio_spf * 2 bytes]
 *       ...  N   Signed 16-bit PCM, big-endian, mono
 *     ...  M   Video: 4-byte BE orig_size + LZ4 block data
 *
 *   After LZ4 decompress, video payload is flat YCbCr bytes:
 *     Y  plane:  encode_w × encode_h        bytes
 *     Cb plane:  (encode_w / 4) × encode_h  bytes  (4:1:1 horizontal)
 *     Cr plane:  (encode_w / 4) × encode_h  bytes
 *
 *   P-frame delta+128 bias:
 *     result[i] = clamp( prev[i] + payload[i] - 128, 0, 255 )
 *     0x80 = no change.
 *
 * ── Controller mapping (port 0) ───────────────────────────────────────────
 *   Start / B   skip  → returns OVA_SKIP immediately
 *   A           pause / resume
 */

#ifndef OVA_N64_H
#define OVA_N64_H

#include <libdragon.h>

/* ── Result codes ────────────────────────────────────────────────────────── */

typedef enum {
    OVA_END   =  0,   /* playback completed normally (all frames shown)      */
    OVA_SKIP  =  1,   /* user pressed B or Start to skip                     */
    OVA_ERROR = -1,   /* file not found, bad magic, unsupported version,     */
                      /* or fatal read error                                  */
} ova_result_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * ova_set_hud_font  —  optional, call before ova_load if your game has already
 * loaded FONT_BUILTIN_DEBUG_MONO.  Passing the existing font pointer prevents
 * ova_load from calling rdpq_font_load_builtin() a second time, which asserts.
 *
 *   font      Pointer returned by your earlier rdpq_font_load_builtin() call.
 *   slot      The rdpq font slot to use for the HUD (must not conflict with
 *             your game's own registrations; default inside ova is slot 3).
 */
void ova_set_hud_font(rdpq_font_t *font, int slot);

/*
 * ova_load  —  blocking OVA v2 FMV player.
 *
 *   path      DFS-relative path to the .ova file, e.g. "ova/opening.ova".
 *             Place your .ova files under assets/ova/ so mkdfs picks them up.
 *
 *   loop      0 = play once and return OVA_END.
 *             1 = loop indefinitely; B or Start still returns OVA_SKIP.
 *
 *   show_hud  0 = fullscreen video, no overlay.
 *             1 = draw a small HUD bar:  frame / total  fps  [PAUSED]
 *                 Uses FONT_BUILTIN_DEBUG_MONO on rdpq font slot 3.
 *
 * Returns OVA_END, OVA_SKIP, or OVA_ERROR.
 */
ova_result_t ova_load(const char *path, int loop, int show_hud);

#endif /* OVA_N64_H */
