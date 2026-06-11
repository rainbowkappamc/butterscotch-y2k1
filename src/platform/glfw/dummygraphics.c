#include "dummygraphics.h"
#include <glad/glad.h>
#include <string.h>
#include <stdio.h>

#include "../../assets/engine/fallback/dummy.h"

static const char* vert = "#version 150\nin vec2 p;in vec2 u;out vec2 v;void main(){gl_Position=vec4(p,0,1);v=u;}";
static const char* frag = "#version 150\nin vec2 v;out vec4 c;uniform sampler2D t;void main(){c=texture(t,v);}";

static GLuint prog = 0, vao = 0, vbo = 0, tex = 0;

static GLuint make_shader(void) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vert, NULL); glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &frag, NULL); glCompileShader(fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "p"); glBindAttribLocation(p, 1, "u");
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

void DummyGraphics_init(int screenW, int screenH) {
    (void)screenW; (void)screenH;
    prog = make_shader();

    float quad[] = { -1,1, 0,0,  1,1, 1,0,  -1,-1, 0,1,  1,-1, 1,1 };
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glBindVertexArray(0);

    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DUMMY_WIDTH, DUMMY_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, dummy_rgba16);
    /* leave bound — unit 0 must never be null while in INIT */
}

void DummyGraphics_shutdown(void) {
    if (tex)  { glDeleteTextures(1, &tex);     tex  = 0; }
    if (vbo)  { glDeleteBuffers(1, &vbo);       vbo  = 0; }
    if (vao)  { glDeleteVertexArrays(1, &vao);  vao  = 0; }
    if (prog) { glDeleteProgram(prog);           prog = 0; }
}

void loadimage(const char* name, int x, int y, int z) {
    (void)x; (void)y; (void)z;
    if (strcmp(name, "dummy") != 0) {
        fprintf(stderr, "dummygraphics: unknown image '%s'\n", name);
        return;
    }
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(prog, "t"), 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}
