#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "runner.h"
#include "audio_system.h"

#ifndef USE_GLFW2
#include <GLFW/glfw3.h>
#else
#include <GL/glfw.h>
#endif

typedef enum {
    DEBUG_ACTION_NONE,
    DEBUG_ACTION_RESTART,
    DEBUG_ACTION_ENTER_ROOM
} DebugAction;

typedef struct {
    DebugAction action;
    int32_t     roomIndex;
} DebugResult;

/* Main entry point. Runs the debug menu loop until the user exits.
   argc/argv are needed for process restart. */
DebugResult DebugScreen_run(Runner* runner, GLFWwindow* window,
                             AudioSystem* audioSystem,
                             int argc, char** argv);
