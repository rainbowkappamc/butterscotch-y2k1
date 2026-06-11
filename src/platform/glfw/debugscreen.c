/* debugscreen.c
 * Debug menu: Sound Test, Graphic Test, Room Test.
 * Lives alongside the other GLFW platform files in src/platform/glfw/. */

#include <glad/glad.h>
#include "debugscreen.h"
#include "gl3d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#include "debug_font/debug_font.h"
#include "stb_image.h"
#include "data_win.h"
#include "runner_gamepad.h"
#include "glfw_gamepad.h"
#include "../../assets/engine/fallback/dummy.h"

#include "miniaudio.h"

static ma_engine s_ma_engine;
static bool      s_ma_ready = false;

/* =========================================================
   Compile-time registry of known .h image assets
   Add new entries here when a new .h image header is added.
   ========================================================= */

typedef enum { H_FMT_RGBA16_5551, H_FMT_RGBA8 } HImageFmt;

typedef struct {
    const char*    filename;   /* just the filename, e.g. "dummy.h" */
    const void*    data;
    int            w, h;
    HImageFmt      fmt;
} HImageEntry;

static const HImageEntry H_IMAGE_REGISTRY[] = {
    { "dummy.h", dummy_rgba16, DUMMY_WIDTH, DUMMY_HEIGHT, H_FMT_RGBA16_5551 },
};
#define H_IMAGE_REGISTRY_COUNT (int)(sizeof(H_IMAGE_REGISTRY)/sizeof(H_IMAGE_REGISTRY[0]))

#ifdef _WIN32
#  include <process.h>
#  define db_restart(argv) _execv((argv)[0], (const char* const*)(argv))
#  define strcasecmp _stricmp
#else
#  include <unistd.h>
#  define db_restart(argv) execv((argv)[0], (argv))
#endif

/* =========================================================
   Constants
   ========================================================= */

#define ITEMS_PER_PAGE  10
#define MAX_FILES       512
#define FONT_SCALE      0.55f
#define LINE_H          (DEBUGFONT_LINE_HEIGHT * FONT_SCALE)
#define PAD             12.0f

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
   GL state (owned by this module)
   ========================================================= */

static GLuint g_prog     = 0;
static GLuint g_vao      = 0;
static GLuint g_vbo      = 0;
static GLuint g_font_tex = 0;
static GLuint g_white_tex= 0;

static GLint  g_u_tex       = -1;
static GLint  g_u_color     = -1;
static GLint  g_u_font_mode = -1;

/* =========================================================
   Shader compilation helpers
   ========================================================= */

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
        fprintf(stderr, "debugscreen shader error: %s\n", buf);
    }
    return s;
}

static GLuint build_program(void) {
    const char* vert =
        "#version 150\n"
        "in vec2 a_pos; in vec2 a_uv; out vec2 v_uv;\n"
        "void main(){ gl_Position=vec4(a_pos,0.0,1.0); v_uv=a_uv; }\n";
    const char* frag =
        "#version 150\n"
        "in vec2 v_uv; out vec4 f_color;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "uniform int u_font_mode;\n" /* 1 = use R channel as alpha (font) */
        "void main(){\n"
        "  if(u_font_mode==1){\n"
        "    float a=texture(u_tex,v_uv).r;\n"
        "    f_color=vec4(u_color.rgb, a*u_color.a);\n"
        "  } else {\n"
        "    f_color=texture(u_tex,v_uv)*u_color;\n"
        "  }\n"
        "}\n";
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag);
    GLuint p  = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

/* =========================================================
   GL state save / restore
   ========================================================= */

typedef struct {
    GLint program;
    GLint vao;
    GLint array_buffer;
    GLint texture;
    GLint active_texture;
    GLboolean blend;
    GLint blend_src_rgb;
    GLint blend_dst_rgb;
    GLint blend_src_alpha;
    GLint blend_dst_alpha;
    GLint unpack_alignment;
} SavedGLState;

static SavedGLState g_saved_gl;

static void gl_state_save(void) {
    glGetIntegerv(GL_CURRENT_PROGRAM,        &g_saved_gl.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,   &g_saved_gl.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,   &g_saved_gl.array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE,         &g_saved_gl.active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,     &g_saved_gl.texture);
    g_saved_gl.blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB,          &g_saved_gl.blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB,          &g_saved_gl.blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,        &g_saved_gl.blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA,        &g_saved_gl.blend_dst_alpha);
    glGetIntegerv(GL_UNPACK_ALIGNMENT,       &g_saved_gl.unpack_alignment);
}

static void gl_state_restore(void) {
    glUseProgram(g_saved_gl.program);
    glBindVertexArray(g_saved_gl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_saved_gl.array_buffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_saved_gl.texture);
    glActiveTexture((GLenum)g_saved_gl.active_texture);
    if (g_saved_gl.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate(g_saved_gl.blend_src_rgb, g_saved_gl.blend_dst_rgb,
                        g_saved_gl.blend_src_alpha, g_saved_gl.blend_dst_alpha);
    glPixelStorei(GL_UNPACK_ALIGNMENT, g_saved_gl.unpack_alignment);
}

static void gl_setup(void) {
    gl_state_save();
    g_prog = build_program();

    g_u_tex       = glGetUniformLocation(g_prog, "u_tex");
    g_u_color     = glGetUniformLocation(g_prog, "u_color");
    g_u_font_mode = glGetUniformLocation(g_prog, "u_font_mode");

    /* Dynamic quad VAO/VBO */
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glBindVertexArray(0);

    /* Font texture — GL_RED, 256x256 */
    glGenTextures(1, &g_font_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED,
                 DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H,
                 0, GL_RED, GL_UNSIGNED_BYTE, debugFontPixels);

    /* 1×1 white texture for solid rects */
    static const uint8_t white[4] = {255,255,255,255};
    glGenTextures(1, &g_white_tex);
    glBindTexture(GL_TEXTURE_2D, g_white_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gl_teardown(void) {
    if (g_font_tex)  { glDeleteTextures(1, &g_font_tex);     g_font_tex  = 0; }
    if (g_white_tex) { glDeleteTextures(1, &g_white_tex);    g_white_tex = 0; }
    if (g_vbo)       { glDeleteBuffers(1, &g_vbo);            g_vbo       = 0; }
    if (g_vao)       { glDeleteVertexArrays(1, &g_vao);       g_vao       = 0; }
    if (g_prog)      { glDeleteProgram(g_prog);               g_prog      = 0; }
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
    gl_state_restore();

    if (s_ma_ready) { ma_engine_uninit(&s_ma_engine); s_ma_ready = false; }
}

/* =========================================================
   Low-level draw: single quad
   ========================================================= */

static void draw_quad(float px, float py, float pw, float ph,
                      float u0, float v0, float u1, float v1,
                      int scrW, int scrH) {
    float nx0 = (px        / scrW) * 2.0f - 1.0f;
    float ny0 = 1.0f - (py        / scrH) * 2.0f;
    float nx1 = ((px+pw)   / scrW) * 2.0f - 1.0f;
    float ny1 = 1.0f - ((py+ph)   / scrH) * 2.0f;
    float v[] = { nx0,ny0, u0,v0,  nx1,ny0, u1,v0,
                  nx1,ny1, u1,v1,  nx0,ny1, u0,v1 };
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* =========================================================
   Draw helpers: rect and text
   ========================================================= */

static void draw_rect(float x, float y, float w, float h,
                      float r, float g, float b, float a,
                      int scrW, int scrH) {
    glUseProgram(g_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_white_tex);
    glUniform1i(g_u_tex, 0);
    glUniform4f(g_u_color, r, g, b, a);
    glUniform1i(g_u_font_mode, 0);
    draw_quad(x, y, w, h, 0,0,1,1, scrW, scrH);
}

/* Returns the x position after the last character. */
static float draw_text(const char* str, float x, float y, float scale,
                       float r, float g, float b, float a,
                       int scrW, int scrH) {
    glUseProgram(g_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(g_u_tex, 0);
    glUniform4f(g_u_color, r, g, b, a);
    glUniform1i(g_u_font_mode, 1);

    float pen = x;
    for (const char* p = str; *p; p++) {
        int c = (unsigned char)*p;
        if (c == ' ') { pen += 12.0f * scale; continue; }
        if (c < DEBUGFONT_FIRST_CP || c > DEBUGFONT_LAST_CP) continue;
        const DebugFontGlyphEntry* gl = &debugFontGlyphs[c - DEBUGFONT_FIRST_CP];
        if (gl->w == 0 || gl->h == 0) { pen += gl->xadvance * scale; continue; }
        float dx = pen + gl->xoffset * scale;
        float dy = y   + gl->yoffset * scale;
        float u0 = gl->x / (float)DEBUGFONT_ATLAS_W;
        float v0 = gl->y / (float)DEBUGFONT_ATLAS_H;
        float u1 = (gl->x + gl->w) / (float)DEBUGFONT_ATLAS_W;
        float v1 = (gl->y + gl->h) / (float)DEBUGFONT_ATLAS_H;
        draw_quad(dx, dy, gl->w * scale, gl->h * scale, u0,v0,u1,v1, scrW, scrH);
        pen += gl->xadvance * scale;
    }
    return pen;
}

static void draw_image_tex(GLuint tex, float x, float y, float disp_w, float disp_h,
                           float img_w, float img_h, int scrW, int scrH) {
    float scale = fminf(disp_w / img_w, disp_h / img_h);
    float w = img_w * scale;
    float h = img_h * scale;
    float ox = x + (disp_w - w) * 0.5f;
    float oy = y + (disp_h - h) * 0.5f;
    glUseProgram(g_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(g_u_tex, 0);
    glUniform4f(g_u_color, 1,1,1,1);
    glUniform1i(g_u_font_mode, 0);
    draw_quad(ox, oy, w, h, 0,0,1,1, scrW, scrH);
}

/* =========================================================
   Input state (edge-detected)
   ========================================================= */

typedef struct {
    bool up, down, select, back;
    bool _up, _down, _select, _back; /* previous frame */
} Input;

static Input s_inp;

static void input_update(GLFWwindow* win) {
    s_inp._up     = s_inp.up;
    s_inp._down   = s_inp.down;
    s_inp._select = s_inp.select;
    s_inp._back   = s_inp.back;

    bool kb_up   = glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS
                || glfwGetKey(win, GLFW_KEY_UP) == GLFW_PRESS;
    bool kb_down = glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS
                || glfwGetKey(win, GLFW_KEY_DOWN) == GLFW_PRESS;
    bool kb_sel  = glfwGetKey(win, GLFW_KEY_ENTER) == GLFW_PRESS
                || glfwGetKey(win, GLFW_KEY_KP_ENTER) == GLFW_PRESS;
    bool kb_back = glfwGetKey(win, GLFW_KEY_BACKSPACE) == GLFW_PRESS
                || glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS;

#ifndef USE_GLFW2
    GLFWgamepadstate gp; int has_gp = glfwGetGamepadState(GLFW_JOYSTICK_1, &gp);
    s_inp.up     = kb_up    || (has_gp && (gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP]
                                           || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.5f));
    s_inp.down   = kb_down  || (has_gp && (gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN]
                                           || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] >  0.5f));
    s_inp.select = kb_sel   || (has_gp && gp.buttons[GLFW_GAMEPAD_BUTTON_A]);
    s_inp.back   = kb_back  || (has_gp && gp.buttons[GLFW_GAMEPAD_BUTTON_B]);
#else
    s_inp.up     = kb_up;
    s_inp.down   = kb_down;
    s_inp.select = kb_sel;
    s_inp.back   = kb_back;
#endif
}

#define PRESSED_UP     (s_inp.up     && !s_inp._up)
#define PRESSED_DOWN   (s_inp.down   && !s_inp._down)
#define PRESSED_SELECT (s_inp.select && !s_inp._select)
#define PRESSED_BACK   (s_inp.back   && !s_inp._back)

/* =========================================================
   File scanning
   ========================================================= */

static bool has_ext(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static bool is_audio(const char* name) {
    return has_ext(name,".mp3") || has_ext(name,".ogg")
        || has_ext(name,".wav") || has_ext(name,".flac");
}

static bool is_image(const char* name) {
    return has_ext(name,".png") || has_ext(name,".jpg")
        || has_ext(name,".jpeg")|| has_ext(name,".bmp");
}

static char s_audio_scan_label[64];
static void scan_audio_dir(const char* dir, int depth) {
    if (depth > 4) return;
    if (depth == 0) snprintf(s_audio_scan_label, sizeof(s_audio_scan_label), "external/%s/", dir);
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && s_audio_n < MAX_FILES) {
        if (e->d_name[0] == '.') continue;
        char full[512]; snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st; if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan_audio_dir(full, depth+1); }
        else if (is_audio(e->d_name)) {
            strncpy(s_audio[s_audio_n].path, full, 511);
            strncpy(s_audio[s_audio_n].name, e->d_name, 255);
            strncpy(s_audio[s_audio_n].source_label, s_audio_scan_label, 63);
            s_audio[s_audio_n].is_internal   = false;
            s_audio[s_audio_n].internal_idx  = -1;
            s_audio_n++;
        }
    }
    closedir(d);
}

static char s_gfx_scan_label[64];
static void scan_gfx_dir(const char* dir, int depth) {
    if (depth > 4) return;
    if (depth == 0) snprintf(s_gfx_scan_label, sizeof(s_gfx_scan_label), "external/%s/", dir);
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && s_gfx_n < MAX_FILES) {
        if (e->d_name[0] == '.') continue;
        char full[512]; snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st; if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan_gfx_dir(full, depth+1); }
        else if (is_image(e->d_name)) {
            strncpy(s_gfx[s_gfx_n].path, full, 511);
            strncpy(s_gfx[s_gfx_n].name, e->d_name, 255);
            strncpy(s_gfx[s_gfx_n].source_label, s_gfx_scan_label, 63);
            s_gfx[s_gfx_n].is_internal  = false;
            s_gfx[s_gfx_n].internal_idx = -1;
            s_gfx[s_gfx_n].is_h_file    = false;
            s_gfx[s_gfx_n].h_reg_idx    = -1;
            s_gfx_n++;
        }
    }
    closedir(d);
}

static void scan_h_dir(const char* dir, int depth) {
    if (depth > 4) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && s_gfx_n < MAX_FILES) {
        if (e->d_name[0] == '.') continue;
        char full[512]; snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st; if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan_h_dir(full, depth+1); continue; }
        if (!has_ext(e->d_name, ".h")) continue;
        /* Look up in registry */
        for (int i = 0; i < H_IMAGE_REGISTRY_COUNT; i++) {
            if (strcasecmp(e->d_name, H_IMAGE_REGISTRY[i].filename) == 0) {
                snprintf(s_gfx[s_gfx_n].name, 256, "[h] %s", e->d_name);
                strncpy(s_gfx[s_gfx_n].path, full, 511);
                s_gfx[s_gfx_n].is_internal  = false;
                s_gfx[s_gfx_n].internal_idx = -1;
                s_gfx[s_gfx_n].is_h_file    = true;
                s_gfx[s_gfx_n].h_reg_idx    = i;
                s_gfx_n++;
                break;
            }
        }
    }
    closedir(d);
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

static void add_txtr_pages(DataWin* dw) {
    for (uint32_t i = 0; i < dw->txtr.count && s_gfx_n < MAX_FILES; i++) {
        Texture* t = &dw->txtr.textures[i];
        if (!t->blobData || t->blobSize == 0) continue;
        snprintf(s_gfx[s_gfx_n].name, 256, "[int] txtr_page_%u", i);
        s_gfx[s_gfx_n].path[0]      = '\0';
        strncpy(s_gfx[s_gfx_n].source_label, "internal/assets/", 63);
        s_gfx[s_gfx_n].is_internal  = true;
        s_gfx[s_gfx_n].internal_idx = (int32_t)i;
        s_gfx[s_gfx_n].is_h_file    = false;
        s_gfx[s_gfx_n].h_reg_idx    = -1;
        s_gfx_n++;
    }
}

static void build_file_lists(DataWin* dw) {
    s_audio_n = 0; s_gfx_n = 0;

    /* Audio: external dirs only */
    scan_audio_dir("assets",     0);
    scan_audio_dir("alt_assets", 0);

    /* Graphics: external dirs + compiled-in .h registry */
    scan_gfx_dir("assets",     0);
    add_h_registry();
    scan_gfx_dir("alt_assets", 0);

    (void)dw;
}

/* =========================================================
   Swap helper
   ========================================================= */

static void db_swap(GLFWwindow* win) {
#ifdef USE_GLFW2
    (void)win; glfwSwapBuffers();
#else
    glfwSwapBuffers(win);
#endif
}

static bool db_should_close(GLFWwindow* win) {
#ifdef USE_GLFW2
    return !glfwGetWindowParam(GLFW_OPENED);
#else
    return glfwWindowShouldClose(win) != 0;
#endif
}

/* =========================================================
   Sound Test
   ========================================================= */

/* Returns true if we should exit the whole debug screen (restart). */
static bool screen_sound(GLFWwindow* win, AudioSystem* audio, int scrW, int scrH) {
    (void)audio;
    int sel = 0, scroll = 0;
    ma_sound s_sound;
    bool     s_sound_loaded = false;

    if (!s_ma_ready) {
        ma_engine_config cfg = ma_engine_config_init();
        if (ma_engine_init(&cfg, &s_ma_engine) == MA_SUCCESS)
            s_ma_ready = true;
    }

    memset(&s_inp, 0, sizeof(s_inp));

    while (!db_should_close(win)) {
        glfwPollEvents();
        input_update(win);

        if (PRESSED_UP)   { sel--; if (sel < 0) sel = 0; }
        if (PRESSED_DOWN) { sel++; if (sel >= s_audio_n) sel = s_audio_n-1; }

        /* Keep sel in view */
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + ITEMS_PER_PAGE) scroll = sel - ITEMS_PER_PAGE + 1;

        if (PRESSED_SELECT && s_audio_n > 0 && s_ma_ready) {
            if (s_sound_loaded) { ma_sound_stop(&s_sound); ma_sound_uninit(&s_sound); s_sound_loaded = false; }
            FileEntry* fe = &s_audio[sel];
            if (ma_sound_init_from_file(&s_ma_engine, fe->path, MA_SOUND_FLAG_ASYNC, NULL, NULL, &s_sound) == MA_SUCCESS) {
                ma_sound_set_looping(&s_sound, MA_FALSE);
                ma_sound_start(&s_sound);
                s_sound_loaded = true;
            }
        }

        if (PRESSED_BACK) {
            if (s_sound_loaded) { ma_sound_stop(&s_sound); ma_sound_uninit(&s_sound); s_sound_loaded = false; }
            return false;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);

        draw_text("Sound Test", PAD, PAD, FONT_SCALE*1.2f, 1,1,0,1, scrW, scrH);
        draw_rect(0, LINE_H*1.4f+PAD, (float)scrW, 1, 0.4f,0.4f,0.4f,1, scrW, scrH);

        if (s_audio_n == 0) {
            draw_text("No audio files found.", PAD, LINE_H*2+PAD, FONT_SCALE, 0.7f,0.7f,0.7f,1, scrW, scrH);
        } else {
            int end = scroll + ITEMS_PER_PAGE;
            if (end > s_audio_n) end = s_audio_n;
            for (int i = scroll; i < end; i++) {
                float iy = LINE_H*2 + PAD + (i - scroll) * (LINE_H + 2);
                bool is_sel = (i == sel);
                if (is_sel) draw_rect(0, iy-2, (float)scrW, LINE_H+4, 0.2f,0.2f,0.5f,1, scrW, scrH);
                float fr = is_sel ? 1.0f : 0.8f, fg = is_sel ? 1.0f : 0.8f, fb = is_sel ? 1.0f : 0.8f;
                /* prefix for internal sounds */
                char label[280];
                if (s_audio[i].is_internal) snprintf(label, sizeof(label), "[int] %s", s_audio[i].name);
                else                         snprintf(label, sizeof(label), "%s",       s_audio[i].name);
                draw_text(label, PAD, iy, FONT_SCALE, fr,fg,fb,1, scrW, scrH);
                /* playing indicator */
                if (i == sel && s_sound_loaded && ma_sound_is_playing(&s_sound))
                    draw_text(">>", (float)scrW - 40, iy, FONT_SCALE, 0,1,0,1, scrW, scrH);
            }
            /* page indicator */
            char pg[64]; snprintf(pg, sizeof(pg), "%d-%d / %d", scroll+1, end, s_audio_n);
            draw_text(pg, PAD, (float)scrH - LINE_H - PAD, FONT_SCALE*0.8f, 0.5f,0.5f,0.5f,1, scrW, scrH);
        }

        draw_text("[W/S] Navigate  [Enter] Play  [Backspace/B] Back",
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
    if (s_sound_loaded) { ma_sound_stop(&s_sound); ma_sound_uninit(&s_sound); }
    return true; /* window closed */
}

/* =========================================================
   Graphic Test
   ========================================================= */

static GLuint load_tex_from_file(const char* path, int* outW, int* outH) {
    int w, h, ch;
    unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) return 0;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    *outW = w; *outH = h;
    return tex;
}

static GLuint load_tex_from_memory(const uint8_t* blob, uint32_t size, int* outW, int* outH) {
    int w, h, ch;
    unsigned char* data = stbi_load_from_memory(blob, (int)size, &w, &h, &ch, 4);
    if (!data) return 0;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    *outW = w; *outH = h;
    return tex;
}

static GLuint load_dummy_tex(int* outW, int* outH) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DUMMY_WIDTH, DUMMY_HEIGHT,
                 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, dummy_rgba16);
    *outW = DUMMY_WIDTH; *outH = DUMMY_HEIGHT;
    return tex;
}

static bool screen_graphic(GLFWwindow* win, DataWin* dw, int scrW, int scrH) {
    int sel = 0, scroll = 0;
    GLuint cur_tex = 0; int cur_w = 0, cur_h = 0;
    int cur_idx = -1;
    bool use_dummy = (s_gfx_n == 0);

    if (use_dummy) { cur_tex = load_dummy_tex(&cur_w, &cur_h); cur_idx = -2; }

    memset(&s_inp, 0, sizeof(s_inp));
    float half = scrW * 0.5f;

    while (!db_should_close(win)) {
        glfwPollEvents();
        input_update(win);

        if (!use_dummy) {
            if (PRESSED_UP)   { sel--; if (sel < 0) sel = 0; }
            if (PRESSED_DOWN) { sel++; if (sel >= s_gfx_n) sel = s_gfx_n-1; }
            if (sel < scroll) scroll = sel;
            if (sel >= scroll + ITEMS_PER_PAGE) scroll = sel - ITEMS_PER_PAGE + 1;

            if (sel != cur_idx) {
                if (cur_tex) { glDeleteTextures(1, &cur_tex); cur_tex = 0; }
                FileEntry* fe = &s_gfx[sel];
                if (fe->is_h_file) {
                    const HImageEntry* he = &H_IMAGE_REGISTRY[fe->h_reg_idx];
                    glGenTextures(1, &cur_tex);
                    glBindTexture(GL_TEXTURE_2D, cur_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    if (he->fmt == H_FMT_RGBA16_5551)
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, he->w, he->h, 0,
                                     GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, he->data);
                    else
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, he->w, he->h, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, he->data);
                    cur_w = he->w; cur_h = he->h;
                } else {
                    cur_tex = load_tex_from_file(fe->path, &cur_w, &cur_h);
                }
                if (!cur_tex) { cur_tex = load_dummy_tex(&cur_w, &cur_h); }
                cur_idx = sel;
            }
        }

        if (PRESSED_BACK) {
            if (cur_tex) glDeleteTextures(1, &cur_tex);
            return false;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);

        /* Left panel: file list */
        draw_text("Graphic Test", PAD, PAD, FONT_SCALE*1.2f, 1,1,0,1, scrW, scrH);
        draw_rect(half-1, 0, 1, (float)scrH, 0.3f,0.3f,0.3f,1, scrW, scrH); /* divider */

        if (use_dummy) {
            draw_text("No image files found. Showing dummy.h", PAD, LINE_H*2+PAD, FONT_SCALE*0.8f, 0.6f,0.6f,0.6f,1, scrW, scrH);
        } else {
            int end = scroll + ITEMS_PER_PAGE;
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
        if (cur_tex) {
            draw_image_tex(cur_tex, half + PAD, PAD,
                           half - PAD*2, (float)scrH - PAD*2,
                           (float)cur_w, (float)cur_h, scrW, scrH);
        }

        draw_text("[W/S] Navigate  [Backspace/B] Back",
                  PAD, (float)scrH - PAD - LINE_H*0.5f, FONT_SCALE*0.7f, 0.4f,0.4f,0.4f,1, scrW, scrH);

        /* Source label bottom right */
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

        db_swap(win);
    }
    if (cur_tex) glDeleteTextures(1, &cur_tex);
    return true;
}

/* =========================================================
   Room Test
   ========================================================= */

/* =========================================================
   GL3D Test Scene
   ========================================================= */

static void screen_gl3d_scene(GLFWwindow* win) {
    gl3d_init(win);
    memset(&s_inp, 0, sizeof(s_inp));

    double last = glfwGetTime();
    while (!db_should_close(win)) {
        glfwPollEvents();
        input_update(win);

        if (PRESSED_BACK) break;

        double now = glfwGetTime();
        float dt = (float)(now - last);
        last = now;

        gl3d_begin_frame();
        gl3d_draw_spinning_cube(dt);
        gl3d_end_frame(win);
    }

    gl3d_shutdown();
}

static int screen_room(GLFWwindow* win, DataWin* dw, int scrW, int scrH) {
    int sel = 0, scroll = 0;
    int dw_count = dw ? (int)dw->room.count : 0;
    int count = dw_count + 1; /* +1 for hardcoded GL3D scene */
    memset(&s_inp, 0, sizeof(s_inp));

    /* Wait for Enter to be released so it doesn't instantly select room 0 */
    while (!db_should_close(win)) {
        glfwPollEvents();
        if (glfwGetKey(win, GLFW_KEY_ENTER)    != GLFW_PRESS &&
            glfwGetKey(win, GLFW_KEY_KP_ENTER) != GLFW_PRESS) break;
    }
    memset(&s_inp, 0, sizeof(s_inp));

    while (!db_should_close(win)) {
        glfwPollEvents();
        input_update(win);

        if (PRESSED_UP)   { sel--; if (sel < 0) sel = 0; }
        if (PRESSED_DOWN) { sel++; if (sel >= count) sel = count-1; }
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + ITEMS_PER_PAGE) scroll = sel - ITEMS_PER_PAGE + 1;

        if (PRESSED_SELECT && count > 0) {
            if (sel == 0) {
                screen_gl3d_scene(win);
                gl_setup();
                memset(&s_inp, 0, sizeof(s_inp));
            } else {
                return sel - 1; /* offset by 1 for dw rooms */
            }
        }
        if (PRESSED_BACK) return -1; /* back to main menu */

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);

        draw_text("Room Test", PAD, PAD, FONT_SCALE*1.2f, 1,1,0,1, scrW, scrH);
        draw_rect(0, LINE_H*1.4f+PAD, (float)scrW, 1, 0.4f,0.4f,0.4f,1, scrW, scrH);

        {
            int end = scroll + ITEMS_PER_PAGE;
            if (end > count) end = count;
            for (int i = scroll; i < end; i++) {
                float iy = LINE_H*2 + PAD + (i - scroll) * (LINE_H + 2);
                bool is_sel = (i == sel);
                if (is_sel) draw_rect(0, iy-2, (float)scrW, LINE_H+4, 0.2f,0.2f,0.5f,1, scrW, scrH);
                float fr = is_sel?1.0f:0.8f, fg=fr, fb=fr;
                char label[300];
                if (i == 0)
                    snprintf(label, sizeof(label), "[0] GL3D Test Scene");
                else
                    snprintf(label, sizeof(label), "[%d] %s", i, dw && dw->room.rooms[i-1].name ? dw->room.rooms[i-1].name : "?");
                draw_text(label, PAD, iy, FONT_SCALE, fr,fg,fb,1, scrW, scrH);
            }
            char pg[64]; snprintf(pg, sizeof(pg), "%d / %d", sel+1, count);
            draw_text(pg, PAD, (float)scrH - LINE_H - PAD, FONT_SCALE*0.8f, 0.5f,0.5f,0.5f,1, scrW, scrH);
        }

        draw_text("[W/S] Navigate  [Enter] Warp  [Backspace/B] Back",
                  PAD, (float)scrH - PAD - LINE_H*0.5f, FONT_SCALE*0.7f, 0.4f,0.4f,0.4f,1, scrW, scrH);

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

DebugResult DebugScreen_run(Runner* runner, GLFWwindow* window,
                             AudioSystem* audioSystem,
                             int argc, char** argv) {
    (void)argc;
    int scrW, scrH;
#ifdef USE_GLFW2
    glfwGetWindowSize(&scrW, &scrH);
#else
    glfwGetFramebufferSize(window, &scrW, &scrH);
#endif
    if (scrW <= 0) scrW = 640;
    if (scrH <= 0) scrH = 480;

    gl_setup();
    build_file_lists(runner ? runner->dataWin : NULL);

    DebugResult result = { DEBUG_ACTION_NONE, -1 };
    int sel = 0;
    memset(&s_inp, 0, sizeof(s_inp));

    while (!db_should_close(window)) {
        glfwPollEvents();
        input_update(window);

        if (PRESSED_UP)   { sel--; if (sel < 0) sel = 0; }
        if (PRESSED_DOWN) { sel++; if (sel >= MENU_COUNT) sel = MENU_COUNT-1; }

        if (PRESSED_BACK) {
            /* Backspace / B / F1 from main menu = restart */
            gl_teardown();
            db_restart(argv);
            /* if execv fails, fall through and return restart action */
            result.action = DEBUG_ACTION_RESTART;
            return result;
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
                    bool closed = screen_graphic(window, runner ? runner->dataWin : NULL, scrW, scrH);
                    if (closed) goto done;
                    memset(&s_inp, 0, sizeof(s_inp));
                    break;
                }
                case MENU_ROOM: {
                    int room_idx = screen_room(window, runner ? runner->dataWin : NULL, scrW, scrH);
                    if (room_idx >= 0) {
                        gl_teardown();
                        result.action    = DEBUG_ACTION_ENTER_ROOM;
                        result.roomIndex = room_idx;
                        return result;
                    }
                    memset(&s_inp, 0, sizeof(s_inp));
                    break;
                }
                default: break;
            }
        }

        /* Draw main menu */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);

        draw_text("Debug Menu", PAD, PAD, FONT_SCALE*1.4f, 1,1,0,1, scrW, scrH);
        draw_rect(0, LINE_H*1.6f+PAD, (float)scrW, 1, 0.4f,0.4f,0.4f,1, scrW, scrH);

        for (int i = 0; i < MENU_COUNT; i++) {
            float iy = LINE_H*2 + PAD + i * (LINE_H*1.3f);
            bool is_sel = (i == sel);
            if (is_sel) draw_rect(0, iy-4, (float)scrW, LINE_H+8, 0.15f,0.15f,0.4f,1, scrW, scrH);
            float fr = is_sel?1.0f:0.7f, fg=fr, fb=is_sel?1.0f:0.7f;
            char label[64];
            snprintf(label, sizeof(label), "%s %s", is_sel?">" :" ", MENU_LABELS[i]);
            draw_text(label, PAD, iy, FONT_SCALE, fr,fg,fb,1, scrW, scrH);
        }

        draw_text("[W/S] Navigate  [Enter] Select  [Backspace/Esc] Restart",
                  PAD, (float)scrH - PAD - LINE_H*0.5f, FONT_SCALE*0.7f, 0.35f,0.35f,0.35f,1, scrW, scrH);

        db_swap(window);
    }

done:
    gl_teardown();
    return result;
}
