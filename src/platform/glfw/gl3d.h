#pragma once

/* gl3d.h — minimal self-contained OpenGL 1.1 3D renderer
   No modern shaders, no VAOs, no UBOs.
   Fixed-function pipeline only.                          */

#ifndef USE_GLFW2
#include <GLFW/glfw3.h>
#else
#include <GL/glfw.h>
#endif

/* Initialise the renderer for the given window.
   Call once after the GL context is current.           */
void gl3d_init(GLFWwindow* window);

/* Resize the viewport (call from a framebuffer-size callback,
   or whenever the window size changes).                */
void gl3d_resize(int width, int height);

/* Begin a frame: clear colour + depth, set up projection. */
void gl3d_begin_frame(void);

/* Draw a single rainbow triangle in clip space.
   Good for a first "am I alive?" test.               */
void gl3d_draw_test_triangle(void);

/* Draw a spinning colour cube. Pass delta time in seconds. */
void gl3d_draw_spinning_cube(float dt);

/* End a frame: swap buffers.                           */
void gl3d_end_frame(GLFWwindow* window);

/* Tear down — call before destroying the GL context.  */
void gl3d_shutdown(void);
