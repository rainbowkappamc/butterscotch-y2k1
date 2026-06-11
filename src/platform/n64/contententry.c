/* debugscreen.c
 * Debug menu: Sound Test, Graphic Test, Room Test.
 * N64/libdragon port — platform layer swapped from GL/GLFW to rdpq/joypad.
 * Everything kept; GL state, file scanning, audio vtable stubs preserved. */

#include "contententry.h"
#include "debug_font_renderer.h"
#include "debug_font/debug_font.h"
#include "dummy.h"
#include "main.h"
#include "n64_datawin.h"
#include "n64_audio.h"
#include <libdragon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int main(void);

extern uint32_t N64DDDetected;
extern DataWinIndex* g_datawin;

static N64AudioSystem* s_audio_sys = NULL;

typedef enum {
    DEBUG_ACTION_NONE,
    DEBUG_ACTION_RESTART,
    DEBUG_ACTION_ENTER_ROOM
} DebugAction;

typedef struct {
    DebugAction action;
    int32_t     roomIndex;
} DebugResult;

/* =========================================================
   Compile-time registry of known .h image assets
   Add new entries here when a new .h image header is added.
   ========================================================= */

typedef enum { H_FMT_RGBA16_5551, H_FMT_RGBA8 } HImageFmt;

typedef struct {
    const char*    filename;
    const void*    data;
    int            w, h;
    HImageFmt      fmt;
} HImageEntry;

static const HImageEntry H_IMAGE_REGISTRY[] = {
    { "dummy.h", dummy_rgba16, DUMMY_WIDTH, DUMMY_HEIGHT, H_FMT_RGBA16_5551 },
};
#define H_IMAGE_REGISTRY_COUNT (int)(sizeof(H_IMAGE_REGISTRY)/sizeof(H_IMAGE_REGISTRY[0]))

/* =========================================================
   Constants
   ========================================================= */

#define ITEMS_PER_PAGE  10  /* fallback — overridden by items_per_page() */
#define MAX_FILES       512
#define FONT_SCALE      0.55f
#define LINE_H          (DEBUGFONT_LINE_HEIGHT * FONT_SCALE)
#define PAD             12.0f

static int items_per_page(int scrH) {
    float top    = LINE_H * 2.0f + PAD;
    float bottom = (float)scrH - PAD - LINE_H * 1.5f;
    float item_h = LINE_H + 2.0f;
    int n = (int)((bottom - top) / item_h);
    if (n < 1) n = 1;
    return n;
}

/* =========================================================
   File entry list
   ========================================================= */

typedef struct {
    char path[512];
    char name[256];
    char source_label[64];
    bool is_internal;
    int32_t internal_idx;
    bool is_h_file;
    int32_t h_reg_idx;
} FileEntry;

static FileEntry s_audio[MAX_FILES];
static int       s_audio_n = 0;
static FileEntry s_gfx[MAX_FILES];
static int       s_gfx_n = 0;

/* =========================================================
   GL state (owned by this module) — stubs on N64
   ========================================================= */

static int g_prog     = 0;
static int g_vao      = 0;
static int g_vbo      = 0;
static int g_font_tex = 0;
static int g_white_tex= 0;

static int g_u_tex       = -1;
static int g_u_color     = -1;
static int g_u_font_mode = -1;

/* =========================================================
   GL state save / restore — stubs on N64
   ========================================================= */

typedef struct {
    int program;
    int vao;
    int array_buffer;
    int texture;
    int active_texture;
    int blend;
    int blend_src_rgb;
    int blend_dst_rgb;
    int blend_src_alpha;
    int blend_dst_alpha;
    int unpack_alignment;
} SavedGLState;

static SavedGLState g_saved_gl;

static void gl_state_save(void)    { (void)g_saved_gl; }
static void gl_state_restore(void) { (void)g_saved_gl; }

/* =========================================================
   DebugFontRenderer — N64 text rendering
   ========================================================= */

static DebugFontRenderer* g_font = NULL;

static void gl_setup(void) {
    gl_state_save();
    g_prog = g_vao = g_vbo = g_font_tex = g_white_tex = 0;
    g_u_tex = g_u_color = g_u_font_mode = -1;
    if (!g_font)  g_font  = DebugFontRenderer_create();
    if (!s_audio_sys) s_audio_sys = N64AudioSystem_create();
}

static void gl_teardown(void) {
    g_prog = g_vao = g_vbo = g_font_tex = g_white_tex = 0;
    if (g_font)  { DebugFontRenderer_destroy(g_font);  g_font  = NULL; }
    if (s_audio_sys) { N64AudioSystem_destroy(s_audio_sys); s_audio_sys = NULL; }
    gl_state_restore();
}

/* =========================================================
   Low-level draw helpers
   ========================================================= */

static void draw_rect(float x, float y, float w, float h,
                      float r, float g, float b, float a,
                      int scrW, int scrH) {
    (void)scrW; (void)scrH;
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32((uint8_t)(r*255),(uint8_t)(g*255),(uint8_t)(b*255),(uint8_t)(a*255)));
    rdpq_fill_rectangle(x, y, x+w, y+h);
}

static float draw_text(const char* str, float x, float y, float scale,
                       float r, float g, float b, float a,
                       int scrW, int scrH) {
    (void)scrW; (void)scrH;
    if (!g_font || !str) return x;
    color_t col = RGBA32((uint8_t)(r*255),(uint8_t)(g*255),(uint8_t)(b*255),(uint8_t)(a*255));
    DebugFontRenderer_printScaled(g_font, x, y, 0, scale, col, str);
    /* approximate returned x — same logic as PC version */
    float pen = x;
    for (const char* p = str; *p; p++) {
        int c = (unsigned char)*p;
        if (c == ' ') { pen += 12.0f * scale; continue; }
        if (c < DEBUGFONT_FIRST_CP || c > DEBUGFONT_LAST_CP) continue;
        pen += debugFontGlyphs[c - DEBUGFONT_FIRST_CP].xadvance * scale;
    }
    return pen;
}

static void draw_image_surf(surface_t* surf, float x, float y,
                            float disp_w, float disp_h,
                            float img_w, float img_h,
                            int scrW, int scrH) {
    (void)scrW; (void)scrH;
    if (!surf || !surf->buffer) return;
    float scale = fminf(disp_w / img_w, disp_h / img_h);
    float w = img_w * scale;
    float h = img_h * scale;
    float ox = x + (disp_w - w) * 0.5f;
    float oy = y + (disp_h - h) * 0.5f;
    rdpq_set_mode_copy(false);
    rdpq_blitparms_t bp = { .scale_x = scale, .scale_y = scale };
    rdpq_tex_blit(surf, ox, oy, &bp);
}

/* =========================================================
   Input state (edge-detected)
   ========================================================= */

typedef struct {
    bool up, down, select, back;
    bool _up, _down, _select, _back;
} Input;

static Input s_inp;

static void input_update(void* win) {
    (void)win;
    s_inp._up     = s_inp.up;
    s_inp._down   = s_inp.down;
    s_inp._select = s_inp.select;
    s_inp._back   = s_inp.back;

    joypad_buttons_t held = joypad_get_buttons_held(0);
    s_inp.up     = held.d_up;
    s_inp.down   = held.d_down;
    s_inp.select = held.a;
    s_inp.back   = held.b;
}

#define PRESSED_UP     (s_inp.up     && !s_inp._up)
#define PRESSED_DOWN   (s_inp.down   && !s_inp._down)
#define PRESSED_SELECT (s_inp.select && !s_inp._select)
#define PRESSED_BACK   (s_inp.back   && !s_inp._back)

/* =========================================================
   File scanning — DFS on N64, opendir/readdir not available.
   Structures and logic preserved; directory walk stubbed.
   ========================================================= */

static bool has_ext(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    /* strcasecmp not available in all N64 libc — manual compare */
    const char* a = dot; const char* b = ext;
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? *a+32 : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? *b+32 : *b;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool is_audio(const char* name) {
    return has_ext(name,".wav") || has_ext(name,".ogg")
        || has_ext(name,".mp3") || has_ext(name,".flac");
}

static bool is_image(const char* name) {
    return has_ext(name,".png") || has_ext(name,".jpg")
        || has_ext(name,".jpeg")|| has_ext(name,".bmp");
}

static char s_audio_scan_label[64] __attribute__((unused));
static void scan_audio_dir(const char* dir, int depth) {
    /* DFS does not support directory enumeration — stub preserved */
    (void)dir; (void)depth; (void)is_audio;
}

static char s_gfx_scan_label[64] __attribute__((unused));
static void scan_gfx_dir(const char* dir, int depth) {
    /* DFS does not support directory enumeration — stub preserved */
    (void)dir; (void)depth; (void)is_image;
}

static void scan_h_dir(const char* dir, int depth) {
    /* DFS does not support directory enumeration — stub preserved */
    (void)dir; (void)depth;
}

static void add_h_registry(void) {
    for (int i = 0; i < H_IMAGE_REGISTRY_COUNT && s_gfx_n < MAX_FILES; i++) {
        snprintf(s_gfx[s_gfx_n].name, 256, "[h] %s", H_IMAGE_REGISTRY[i].filename);
        s_gfx[s_gfx_n].path[0]      = '\0';
        strncpy(s_gfx[s_gfx_n].source_label, "internal/assets/", 63);
        s_gfx[s_gfx_n].is_internal  = false;
        s_gfx[s_gfx_n].internal_idx = -1;
        s_gfx[s_gfx_n].is_h_file    = true;
        s_gfx[s_gfx_n].h_reg_idx    = i;
        s_gfx_n++;
    }
}

static char** s_room_names  = NULL;
static int    s_room_count  = 0;

static void add_txtr_pages(void* dw) {
    (void)dw;
    if (!g_datawin) return;
    int n = datawin_get_txtr_count(g_datawin);
    for (int i = 0; i < n && s_gfx_n < MAX_FILES; i++) {
        snprintf(s_gfx[s_gfx_n].name, 256, "[int] txtr_page_%d", i);
        s_gfx[s_gfx_n].path[0]      = '\0';
        strncpy(s_gfx[s_gfx_n].source_label, "internal/assets/", 63);
        s_gfx[s_gfx_n].is_internal  = true;
        s_gfx[s_gfx_n].internal_idx = i;
        s_gfx[s_gfx_n].is_h_file    = false;
        s_gfx[s_gfx_n].h_reg_idx    = -1;
        s_gfx_n++;
    }
}

static void build_file_lists(void* dw) {
    s_audio_n = 0; s_gfx_n = 0;

    /* Free previous room names */
    if (s_room_names) {
        for (int i = 0; i < s_room_count; i++) free(s_room_names[i]);
        free(s_room_names);
        s_room_names = NULL;
        s_room_count = 0;
    }

    scan_audio_dir("assets",     0);

    /* Internal sounds from data.win SOND chunk */
    if (g_datawin) {
        DataWinNameList* sounds = datawin_get_sounds(g_datawin);
        if (sounds) {
            for (int i = 0; i < sounds->count && s_audio_n < MAX_FILES; i++) {
                strncpy(s_audio[s_audio_n].name, sounds->names[i] ? sounds->names[i] : "?", 255);
                s_audio[s_audio_n].path[0]      = '\0';
                strncpy(s_audio[s_audio_n].source_label, "internal/assets/", 63);
                s_audio[s_audio_n].is_internal  = true;
                s_audio[s_audio_n].internal_idx = i;
                s_audio_n++;
            }
            datawin_name_list_free(sounds);
        }
    }

    scan_audio_dir("alt_assets", 0);

    scan_gfx_dir("assets",     0);
    scan_h_dir("assets",       0);
    add_h_registry();
    add_txtr_pages(dw);
    scan_gfx_dir("alt_assets", 0);

    /* Room names from data.win ROOM chunk */
    if (g_datawin) {
        DataWinNameList* rooms = datawin_get_rooms(g_datawin);
        if (rooms) {
            s_room_count = rooms->count;
            s_room_names = (char**) malloc(s_room_count * sizeof(char*));
            for (int i = 0; i < s_room_count; i++) {
                s_room_names[i] = rooms->names[i] ? strdup(rooms->names[i]) : strdup("?");
            }
            datawin_name_list_free(rooms);
        }
    }
}

/* =========================================================
   Swap / should-close helpers
   ========================================================= */

static void db_swap(void* win) {
    (void)win;
    rdpq_detach_show();
}

static bool db_should_close(void* win) {
    (void)win;
    return false; /* no window close on N64 */
}

/* =========================================================
   Screen clear helper
   ========================================================= */

static void db_clear(void) {
    surface_t* fb = display_get();
    rdpq_attach_clear(fb, NULL);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_set_prim_color(RGBA32(0,0,0,255));
    int scrW = N64DDDetected ? 640 : 320;
    int scrH = N64DDDetected ? 480 : 240;
    rdpq_fill_rectangle(0, 0, scrW, scrH);
}

/* =========================================================
   Sound Test
   ========================================================= */

static bool screen_sound(void* win, void* audio, int scrW, int scrH) {
    int sel = 0, scroll = 0;
    int32_t playing_instance = -1;
    int32_t playing_stream   = -1;

    memset(&s_inp, 0, sizeof(s_inp));

    while (!db_should_close(win)) {
        joypad_poll();
        input_update(win);

        if (PRESSED_UP)   { sel--; if (sel < 0) sel = 0; }
        if (PRESSED_DOWN) { sel++; if (sel >= s_audio_n) sel = s_audio_n-1; }

        if (sel < scroll) scroll = sel;
        if (sel >= scroll + items_per_page(scrH)) scroll = sel - items_per_page(scrH) + 1;

        if (PRESSED_SELECT && s_audio_n > 0 && s_audio_sys) {
            if (playing_instance >= 0)
                N64AudioSystem_stop(s_audio_sys, playing_instance);

            const char* snd_name = s_audio[sel].name;

            /* Try FM index first via g_fm_audio */
            if (g_fm_audio)
                playing_instance = N64AudioSystem_play_fm(
                    g_fm_audio, snd_name, false, 1.0f, 1.0f);

            /* Fall back to AUDO chunk from data.win */
            if (playing_instance < 0 && g_datawin) {
                int audo_idx = datawin_get_audo_index(g_datawin, sel);
                if (audo_idx >= 0) {
                    DataWinAudoEntry* ae = datawin_get_audo_entry(g_datawin, audo_idx);
                    if (ae) {
                        playing_instance = N64AudioSystem_play_raw(
                            s_audio_sys, sel, ae->data, ae->size, false, 1.0f, 1.0f);
                        free(ae->data);
                        free(ae);
                    }
                }
            }
        }

        if (PRESSED_BACK) {
            if (playing_instance >= 0 && s_audio_sys)
                N64AudioSystem_stop(s_audio_sys, playing_instance);
            return false;
        }

        if (s_audio_sys) N64AudioSystem_update(s_audio_sys);
        if (g_fm_audio)  N64AudioSystem_update(g_fm_audio);

        db_clear();

        draw_text("Sound Test", PAD, PAD, FONT_SCALE*1.2f, 1,1,0,1, scrW, scrH);
        draw_rect(0, LINE_H*1.4f+PAD, (float)scrW, 1, 0.4f,0.4f,0.4f,1, scrW, scrH);

        if (s_audio_n == 0) {
            draw_text("No audio files found.", PAD, LINE_H*2+PAD, FONT_SCALE, 0.7f,0.7f,0.7f,1, scrW, scrH);
        } else {
            int end = scroll + items_per_page(scrH);
            if (end > s_audio_n) end = s_audio_n;
            for (int i = scroll; i < end; i++) {
                float iy = LINE_H*2 + PAD + (i - scroll) * (LINE_H + 2);
                bool is_sel = (i == sel);
                if (is_sel) draw_rect(0, iy-2, (float)scrW, LINE_H+4, 0.2f,0.2f,0.5f,1, scrW, scrH);
                float fr = is_sel ? 1.0f : 0.8f, fg = fr, fb = fr;
                char label[280];
                if (s_audio[i].is_internal) snprintf(label, sizeof(label), "[int] %s", s_audio[i].name);
                else                         snprintf(label, sizeof(label), "%s",       s_audio[i].name);
                draw_text(label, PAD, iy, FONT_SCALE, fr,fg,fb,1, scrW, scrH);
                if (i == sel && playing_instance >= 0)
                    draw_text(">>", (float)scrW - 40, iy, FONT_SCALE, 0,1,0,1, scrW, scrH);
            }
            char pg[64]; snprintf(pg, sizeof(pg), "%d-%d / %d", scroll+1, end, s_audio_n);
            draw_text(pg, PAD, (float)scrH - LINE_H - PAD, FONT_SCALE*0.8f, 0.5f,0.5f,0.5f,1, scrW, scrH);
        }

        draw_text("[D-Up/Down] Navigate  [A] Play  [B] Back",
                  PAD, (float)scrH - PAD - LINE_H*0.5f, FONT_SCALE*0.7f, 0.4f,0.4f,0.4f,1, scrW, scrH);

        if (s_audio_n > 0) {
            const char* lbl = s_audio[sel].source_label;
            float lw = 0;
            for (const char* p = lbl; *p; p++) {
                int c = (unsigned char)*p;
                if (c >= DEBUGFONT_FIRST_CP && c <= DEBUGFONT_LAST_CP)
                    lw += debugFontGlyphs[c - DEBUGFONT_FIRST_CP].xadvance * (FONT_SCALE*0.7f);
                else lw += 12.0f * (FONT_SCALE*0.7f);
            }
            draw_text(lbl, (float)scrW - lw - PAD, (float)scrH - PAD - LINE_H*0.5f,
                      FONT_SCALE*0.7f, 0.35f,0.7f,0.35f,1, scrW, scrH);
        }

        db_swap(win);
    }
    (void)playing_instance; (void)playing_stream;
    return true;
}

/* =========================================================
   Graphic Test
   ========================================================= */

static surface_t load_surf_from_h(int reg_idx) {
    const HImageEntry* he = &H_IMAGE_REGISTRY[reg_idx];
    surface_t s = surface_make_linear((void*)he->data, FMT_RGBA16, he->w, he->h);
    return s;
}

static bool screen_graphic(void* win, void* dw, int scrW, int scrH) {
    int sel = 0, scroll = 0;
    surface_t cur_surf = {0}; int cur_w = 0, cur_h = 0;
    int cur_idx = -1;
    bool use_dummy = (s_gfx_n == 0);

    if (use_dummy) {
        cur_surf = surface_make_linear((void*)dummy_rgba16, FMT_RGBA16, DUMMY_WIDTH, DUMMY_HEIGHT);
        cur_w = DUMMY_WIDTH; cur_h = DUMMY_HEIGHT;
        cur_idx = -2;
    }

    memset(&s_inp, 0, sizeof(s_inp));
    float half = scrW * 0.5f;

    while (!db_should_close(win)) {
        joypad_poll();
        input_update(win);

        if (!use_dummy) {
            if (PRESSED_UP)   { sel--; if (sel < 0) sel = 0; }
            if (PRESSED_DOWN) { sel++; if (sel >= s_gfx_n) sel = s_gfx_n-1; }
            if (sel < scroll) scroll = sel;
            if (sel >= scroll + items_per_page(scrH)) scroll = sel - items_per_page(scrH) + 1;

            if (sel != cur_idx) {
                FileEntry* fe = &s_gfx[sel];
                if (fe->is_h_file) {
                    cur_surf = load_surf_from_h(fe->h_reg_idx);
                    cur_w = H_IMAGE_REGISTRY[fe->h_reg_idx].w;
                    cur_h = H_IMAGE_REGISTRY[fe->h_reg_idx].h;
                } else {
                    /* External/internal file loading not available on N64 — show dummy */
                    cur_surf = surface_make_linear((void*)dummy_rgba16, FMT_RGBA16, DUMMY_WIDTH, DUMMY_HEIGHT);
                    cur_w = DUMMY_WIDTH; cur_h = DUMMY_HEIGHT;
                }
                cur_idx = sel;
            }
        }

        if (PRESSED_BACK) {
            return false;
        }

        db_clear();

        /* Left panel: file list */
        draw_text("Graphic Test", PAD, PAD, FONT_SCALE*1.2f, 1,1,0,1, scrW, scrH);
        draw_rect(half-1, 0, 1, (float)scrH, 0.3f,0.3f,0.3f,1, scrW, scrH);

        if (use_dummy) {
            draw_text("No image files found. Showing dummy.h", PAD, LINE_H*2+PAD, FONT_SCALE*0.8f, 0.6f,0.6f,0.6f,1, scrW, scrH);
        } else {
            int end = scroll + items_per_page(scrH);
            if (end > s_gfx_n) end = s_gfx_n;
            for (int i = scroll; i < end; i++) {
                float iy = LINE_H*2 + PAD + (i - scroll) * (LINE_H + 2);
                bool is_sel = (i == sel);
                if (is_sel) draw_rect(0, iy-2, half, LINE_H+4, 0.2f,0.2f,0.5f,1, scrW, scrH);
                float fr = is_sel?1.0f:0.8f, fg = fr, fb = fr;
                draw_text(s_gfx[i].name, PAD, iy, FONT_SCALE, fr,fg,fb,1, scrW, scrH);
            }
            char pg[64]; snprintf(pg, sizeof(pg), "%d/%d", sel+1, s_gfx_n);
            draw_text(pg, PAD, (float)scrH - LINE_H - PAD, FONT_SCALE*0.8f, 0.5f,0.5f,0.5f,1, scrW, scrH);
        }

        /* Right panel: image */
        if (cur_surf.buffer) {
            draw_image_surf(&cur_surf, half + PAD, PAD,
                            half - PAD*2, (float)scrH - PAD*2,
                            (float)cur_w, (float)cur_h, scrW, scrH);
        }

        draw_text("[D-Up/Down] Navigate  [B] Back",
                  PAD, (float)scrH - PAD - LINE_H*0.5f, FONT_SCALE*0.7f, 0.4f,0.4f,0.4f,1, scrW, scrH);

        if (!use_dummy && s_gfx_n > 0) {
            const char* lbl = s_gfx[sel].source_label;
            float lw = 0;
            for (const char* p = lbl; *p; p++) {
                int c = (unsigned char)*p;
                if (c >= DEBUGFONT_FIRST_CP && c <= DEBUGFONT_LAST_CP)
                    lw += debugFontGlyphs[c - DEBUGFONT_FIRST_CP].xadvance * (FONT_SCALE*0.7f);
                else lw += 12.0f * (FONT_SCALE*0.7f);
            }
            draw_text(lbl, (float)scrW - lw - PAD, (float)scrH - PAD - LINE_H*0.5f,
                      FONT_SCALE*0.7f, 0.35f,0.7f,0.35f,1, scrW, scrH);
        }

        (void)dw;
        db_swap(win);
    }
    return true;
}

/* =========================================================
   Room Test
   ========================================================= */

static int screen_room(void* win, void* dw, int scrW, int scrH) {
    int sel = 0, scroll = 0;
    int count = s_room_count;
    memset(&s_inp, 0, sizeof(s_inp));

    while (!db_should_close(win)) {
        joypad_poll();
        input_update(win);

        if (PRESSED_UP)   { sel--; if (sel < 0) sel = 0; }
        if (PRESSED_DOWN) { sel++; if (sel >= count) sel = count-1; }
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + items_per_page(scrH)) scroll = sel - items_per_page(scrH) + 1;

        if (PRESSED_SELECT && count > 0) return sel;
        if (PRESSED_BACK) return -1;

        db_clear();

        draw_text("Room Test", PAD, PAD, FONT_SCALE*1.2f, 1,1,0,1, scrW, scrH);
        draw_rect(0, LINE_H*1.4f+PAD, (float)scrW, 1, 0.4f,0.4f,0.4f,1, scrW, scrH);

        if (count == 0) {
            draw_text("No rooms found.", PAD, LINE_H*2+PAD, FONT_SCALE, 0.7f,0.7f,0.7f,1, scrW, scrH);
        } else {
            int end = scroll + items_per_page(scrH);
            if (end > count) end = count;
            for (int i = scroll; i < end; i++) {
                float iy = LINE_H*2 + PAD + (i - scroll) * (LINE_H + 2);
                bool is_sel = (i == sel);
                if (is_sel) draw_rect(0, iy-2, (float)scrW, LINE_H+4, 0.2f,0.2f,0.5f,1, scrW, scrH);
                float fr = is_sel?1.0f:0.8f, fg=fr, fb=fr;
                char label[300];
                snprintf(label, sizeof(label), "[%d] %s", i,
                         (s_room_names && s_room_names[i]) ? s_room_names[i] : "?");
                draw_text(label, PAD, iy, FONT_SCALE, fr,fg,fb,1, scrW, scrH);
            }
            char pg[64]; snprintf(pg, sizeof(pg), "%d / %d", sel+1, count);
            draw_text(pg, PAD, (float)scrH - LINE_H - PAD, FONT_SCALE*0.8f, 0.5f,0.5f,0.5f,1, scrW, scrH);
        }

        draw_text("[D-Up/Down] Navigate  [A] Warp  [B] Back",
                  PAD, (float)scrH - PAD - LINE_H*0.5f, FONT_SCALE*0.7f, 0.4f,0.4f,0.4f,1, scrW, scrH);

        (void)dw;
        db_swap(win);
    }
    return -1;
}

/* =========================================================
   Main debug menu
   ========================================================= */

typedef enum { MENU_SOUND, MENU_GRAPHIC, MENU_ROOM, MENU_COUNT } MenuItem;
static const char* MENU_LABELS[MENU_COUNT] = {
    "Sound Test",
    "Graphic Test",
    "Room Test",
};

/* =========================================================
   Entry point
   ========================================================= */

void ac_main(void) {
    void* runner __attribute__((unused)) = NULL;
    void* window = NULL;
    void* audioSystem = NULL;

    int scrW = N64DDDetected ? 640 : 320;
    int scrH = N64DDDetected ? 480 : 240;

    gl_setup();
    build_file_lists(NULL);

    DebugResult result = { DEBUG_ACTION_NONE, -1 };
    (void)result;
    int sel = 0;
    memset(&s_inp, 0, sizeof(s_inp));

    /* Play example.fm as background music for the debug menu */
    int32_t menu_music = -1;
    if (g_fm_audio)
        menu_music = N64AudioSystem_play(g_fm_audio, -1,
                                         "assets/contents/audio/fm/example.fm",
                                         true, 1.0f, 1.0f);

    while (!db_should_close(window)) {
        joypad_poll();
        input_update(window);

        if (s_audio_sys) N64AudioSystem_update(s_audio_sys);
        if (s_audio_sys) N64AudioSystem_update(s_audio_sys);
        if (g_fm_audio)  N64AudioSystem_update(g_fm_audio);
        if (PRESSED_UP)   { sel--; if (sel < 0) sel = 0; }
        if (PRESSED_DOWN) { sel++; if (sel >= MENU_COUNT) sel = MENU_COUNT-1; }

        if (PRESSED_BACK) {
            gl_teardown();
            rdpq_close();
            audio_close();
            display_close();
            tick_reset();
            eng_state = ENG_INIT;
            main();
            return;
        }

        if (PRESSED_SELECT) {
            switch ((MenuItem)sel) {
                case MENU_SOUND: {
                    bool closed = screen_sound(window, audioSystem, scrW, scrH);
                    if (closed) goto done;
                    memset(&s_inp, 0, sizeof(s_inp));
                    break;
                }
                case MENU_GRAPHIC: {
                    bool closed = screen_graphic(window, NULL, scrW, scrH);
                    if (closed) goto done;
                    memset(&s_inp, 0, sizeof(s_inp));
                    break;
                }
                case MENU_ROOM: {
                    int room_idx = screen_room(window, NULL, scrW, scrH);
                    if (room_idx >= 0) {
                        gl_teardown();
                        return;
                    }
                    memset(&s_inp, 0, sizeof(s_inp));
                    break;
                }
                default: break;
            }
        }

        /* Draw main menu */
        db_clear();

        draw_text("Debug Menu", PAD, PAD, FONT_SCALE*1.4f, 1,1,0,1, scrW, scrH);
        draw_rect(0, LINE_H*1.6f+PAD, (float)scrW, 1, 0.4f,0.4f,0.4f,1, scrW, scrH);

        for (int i = 0; i < MENU_COUNT; i++) {
            float iy = LINE_H*2 + PAD + i * (LINE_H*1.3f);
            bool is_sel = (i == sel);
            if (is_sel) draw_rect(0, iy-4, (float)scrW, LINE_H+8, 0.15f,0.15f,0.4f,1, scrW, scrH);
            float fr = is_sel?1.0f:0.7f, fg=fr, fb=is_sel?1.0f:0.7f;
            char label[64];
            snprintf(label, sizeof(label), "%s %s", is_sel?">":" ", MENU_LABELS[i]);
            draw_text(label, PAD, iy, FONT_SCALE, fr,fg,fb,1, scrW, scrH);
        }

        draw_text("[D-Up/Down] Navigate  [A] Select  [B] Restart",
                  PAD, (float)scrH - PAD - LINE_H*0.5f, FONT_SCALE*0.7f, 0.35f,0.35f,0.35f,1, scrW, scrH);

        db_swap(window);
    }

done:
    if (g_fm_audio && menu_music >= 0)
        N64AudioSystem_stop(g_fm_audio, menu_music);
    gl_teardown();
}
