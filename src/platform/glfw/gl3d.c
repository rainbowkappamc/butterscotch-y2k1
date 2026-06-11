/* gl3d.c — self-contained 3D renderer, works with Core GL via GLAD */

#include <glad/glad.h>
#include "gl3d.h"
#include <math.h>
#include <string.h>

#ifndef USE_GLFW2
#include <GLFW/glfw3.h>
#else
#include <GL/glfw.h>
#endif

static int   g_width  = 640;
static int   g_height = 480;
static float g_angle  = 0.0f;

static GLuint g_prog = 0;
static GLuint g_vao  = 0;
static GLuint g_vbo  = 0;
static GLuint g_ibo  = 0;

static GLint g_u_mvp = -1;

/* ---- shaders ---- */
static const char* VERT =
    "#version 150\n"
    "in vec3 a_pos;\n"
    "in vec3 a_col;\n"
    "out vec3 v_col;\n"
    "uniform mat4 u_mvp;\n"
    "void main(){\n"
    "  gl_Position = u_mvp * vec4(a_pos,1.0);\n"
    "  v_col = a_col;\n"
    "}\n";

static const char* FRAG =
    "#version 150\n"
    "in vec3 v_col;\n"
    "out vec4 f_color;\n"
    "void main(){ f_color = vec4(v_col,1.0); }\n";

/* ---- cube geometry ---- */
/* 8 vertices: pos(3) + color(3) */
static const float CUBE_VERTS[] = {
    /* x     y     z     r     g     b  */
    -1.0f,-1.0f, 1.0f,  0.9f, 0.2f, 0.2f,
     1.0f,-1.0f, 1.0f,  0.9f, 0.2f, 0.2f,
     1.0f, 1.0f, 1.0f,  0.9f, 0.2f, 0.2f,
    -1.0f, 1.0f, 1.0f,  0.9f, 0.2f, 0.2f,
    -1.0f,-1.0f,-1.0f,  0.2f, 0.9f, 0.2f,
     1.0f,-1.0f,-1.0f,  0.2f, 0.9f, 0.2f,
     1.0f, 1.0f,-1.0f,  0.2f, 0.4f, 0.9f,
    -1.0f, 1.0f,-1.0f,  0.9f, 0.9f, 0.2f,
};
static const unsigned short CUBE_IDX[] = {
    0,1,2, 2,3,0,   /* front  */
    5,4,7, 7,6,5,   /* back   */
    4,0,3, 3,7,4,   /* left   */
    1,5,6, 6,2,1,   /* right  */
    3,2,6, 6,7,3,   /* top    */
    4,5,1, 1,0,4,   /* bottom */
};

/* ---- matrix helpers ---- */
static void mat_identity(float m[16]) {
    memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1.0f;
}
static void mat_mul(float out[16], const float a[16], const float b[16]) {
    float t[16]={0};
    for(int r=0;r<4;r++) for(int c=0;c<4;c++)
        for(int k=0;k<4;k++) t[c*4+r]+=a[k*4+r]*b[c*4+k];
    memcpy(out,t,64);
}
static void mat_perspective(float m[16], float fovy, float aspect, float zn, float zf) {
    float f=1.0f/tanf(fovy*3.14159265f/360.0f);
    memset(m,0,64);
    m[0]=f/aspect; m[5]=f;
    m[10]=(zf+zn)/(zn-zf); m[11]=-1.0f;
    m[14]=(2.0f*zf*zn)/(zn-zf);
}
static void mat_rotate_x(float m[16], float deg) {
    float r=deg*3.14159265f/180.0f, c=cosf(r), s=sinf(r);
    mat_identity(m); m[5]=c; m[6]=s; m[9]=-s; m[10]=c;
}
static void mat_rotate_y(float m[16], float deg) {
    float r=deg*3.14159265f/180.0f, c=cosf(r), s=sinf(r);
    mat_identity(m); m[0]=c; m[2]=-s; m[8]=s; m[10]=c;
}
static void mat_rotate_z(float m[16], float deg) {
    float r=deg*3.14159265f/180.0f, c=cosf(r), s=sinf(r);
    mat_identity(m); m[0]=c; m[1]=s; m[4]=-s; m[5]=c;
}
static void mat_translate_z(float m[16], float z) {
    mat_identity(m); m[14]=z;
}

/* ---- shader compile ---- */
static GLuint compile(GLenum type, const char* src) {
    GLuint s=glCreateShader(type);
    glShaderSource(s,1,&src,NULL);
    glCompileShader(s);
    return s;
}

/* =========================================================
   Init / shutdown
   ========================================================= */

void gl3d_init(GLFWwindow* window) {
    int w,h;
#ifdef USE_GLFW2
    glfwGetWindowSize(&w,&h);
#else
    glfwGetFramebufferSize(window,&w,&h);
#endif
    if(w<=0) w=640; 
    if(h<=0) h=480;
    g_width=w; g_height=h; g_angle=0.0f;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.08f,0.08f,0.08f,1.0f);

    /* program */
    GLuint vs=compile(GL_VERTEX_SHADER,VERT);
    GLuint fs=compile(GL_FRAGMENT_SHADER,FRAG);
    g_prog=glCreateProgram();
    glAttachShader(g_prog,vs); glAttachShader(g_prog,fs);
    glBindAttribLocation(g_prog,0,"a_pos");
    glBindAttribLocation(g_prog,1,"a_col");
    glLinkProgram(g_prog);
    glDeleteShader(vs); glDeleteShader(fs);
    g_u_mvp=glGetUniformLocation(g_prog,"u_mvp");

    /* VAO/VBO/IBO */
    glGenVertexArrays(1,&g_vao);
    glGenBuffers(1,&g_vbo);
    glGenBuffers(1,&g_ibo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER,g_vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(CUBE_VERTS),CUBE_VERTS,GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(CUBE_IDX),CUBE_IDX,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(3*sizeof(float)));
    glBindVertexArray(0);
}

void gl3d_shutdown(void) {
    if(g_vao)  { glDeleteVertexArrays(1,&g_vao); g_vao=0; }
    if(g_vbo)  { glDeleteBuffers(1,&g_vbo);      g_vbo=0; }
    if(g_ibo)  { glDeleteBuffers(1,&g_ibo);      g_ibo=0; }
    if(g_prog) { glDeleteProgram(g_prog);         g_prog=0; }
    glDisable(GL_DEPTH_TEST);
}

void gl3d_resize(int width, int height) {
    if(width<=0) width=1; 
    if(height<=0) height=1;
    g_width=width; g_height=height;
    glViewport(0,0,width,height);
}

void gl3d_begin_frame(void) {
    glViewport(0,0,g_width,g_height);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
}

void gl3d_end_frame(GLFWwindow* window) {
#ifdef USE_GLFW2
    (void)window; glfwSwapBuffers();
#else
    glfwSwapBuffers(window);
#endif
}

void gl3d_draw_spinning_cube(float dt) {
    g_angle += 45.0f*dt;
    if(g_angle>=360.0f) g_angle-=360.0f;

    float proj[16], tz[16], rx[16], ry[16], rz[16], tmp[16], mvp[16];
    float aspect=(g_height>0)?(float)g_width/(float)g_height:1.0f;
    mat_perspective(proj,60.0f,aspect,0.1f,1000.0f);
    mat_translate_z(tz,-3.5f);
    mat_rotate_x(rx, g_angle);
    mat_rotate_y(ry, g_angle*0.7f);
    mat_rotate_z(rz, g_angle*0.3f);
    mat_mul(tmp,rx,ry);
    mat_mul(tmp,tmp,rz);
    mat_mul(tmp,tz,tmp);
    mat_mul(mvp,proj,tmp);

    glUseProgram(g_prog);
    glUniformMatrix4fv(g_u_mvp,1,GL_FALSE,mvp);
    glBindVertexArray(g_vao);
    glDrawElements(GL_TRIANGLES,36,GL_UNSIGNED_SHORT,0);
    glBindVertexArray(0);
    glUseProgram(0);
}

void gl3d_draw_test_triangle(void) {
    gl3d_draw_spinning_cube(0.016f);
}