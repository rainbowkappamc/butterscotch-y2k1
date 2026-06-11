#pragma once

#include <stdint.h>
#include "n64_audio.h"

#define ENG_INIT  0
#define ENG_IDLE  1
#define ENG_HOLD  2
#define ENG_DEBUG 3
#define ENG_CRASH 4

extern int eng_state;

void     tick_update(void);
uint64_t tick_get(void);
void     tick_reset(void);

extern int            g_soft_reset;
extern N64AudioSystem* g_fm_audio;
