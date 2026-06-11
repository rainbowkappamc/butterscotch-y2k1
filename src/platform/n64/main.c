//
// * main.c
// * Engine entry point. Initialises libdragon then hands off to the main game content.
// * Nothing game-specific lives here.

#include <libdragon.h>
#include <string.h>
#include "dummy.h"
#include "contententry.h"
#include "n64_datawin.h"
#include "n64_audio.h"

/* fm_contenttable.h is optional — silently falls back to no FM audio if absent */
#if __has_include("fm_contenttable.h")
#include "fm_contenttable.h"
#define FM_AUDIO_AVAILABLE 1
#else
#define FM_AUDIO_AVAILABLE 0
#define FM_INDEX_COUNT 0
#endif

// Forward declarations //
void     tick_update(void);
uint64_t tick_get(void);
void     tick_reset(void);
void     eng_show_init_image(void);

int g_soft_reset = 0;

/* Asset manager — populated during ENG_INIT, lives for the session */
DataWinIndex*   g_datawin  = NULL;
N64AudioSystem* g_fm_audio = NULL;

//main engine parameters
#define debugprint(msg) debugf("[%s] %s\n", __FILE__, msg)

//display
resolution_t resolution = RESOLUTION_320x240; //320x240 -> bumped up to 480 on N64DD or select cases.
bitdepth_t  colordepth = DEPTH_16_BPP; //16 bit, 24 bit, or 32 bit
uint32_t displaybuffer = 2;
gamma_t displaygamma = GAMMA_NONE; //gamma
antialias_t displayfilter = FILTERS_RESAMPLE; //antialiasing

//filesystem
uint32_t BaseN64Directory = DFS_DEFAULT_LOCATION;
uint32_t MFSN64DDDirectory = 0; //not setup

//engine 
uint32_t N64DDDetected = 0;

void mfs_init(uint32_t directory)
{
    N64DDDetected = 1; //enable flag
    //this is dummy, another script will handle custom 64dd filesystem and 64dd detection.
}

//Global Engine FSM States
#define ENG_INIT  0
#define ENG_IDLE  1
#define ENG_HOLD  2
#define ENG_DEBUG 3
#define ENG_CRASH 4
//Default State
int eng_state = ENG_INIT;

// MAIN.C ENTRY
int main(void) {

    while (1) {
        switch (eng_state) {

            case ENG_INIT:
                debugprint("ENGINE STATE INIT"); 
                //general filesystem init
                dfs_init(BaseN64Directory); 

                //64dd check 1
                if (N64DDDetected != 0)
                {
                    mfs_init(MFSN64DDDirectory);
                    resolution = RESOLUTION_640x480;
                }

                /* Scan data.win — build chunk index and 4 MB window table */
                if (!g_datawin) {
                    g_datawin = datawin_scan("data.win");
                    if (!g_datawin) {
                        debugprint("data.win not found or invalid — asset streaming disabled");
                    } else {
                        debugf("[main] data.win: %d chunks, %d windows\n",
                               g_datawin->chunk_count, g_datawin->window_count);
                    }
                }

                /* FM audio index — built from fm_contenttable.h if present */
                if (!g_fm_audio) {
#if FM_AUDIO_AVAILABLE
                    g_fm_audio = N64AudioSystem_create();
                    debugf("[main] FM audio: %d entries from fm_contenttable.h\n",
                           FM_INDEX_COUNT);
#else
                    debugprint("fm_contenttable.h not found — FM audio disabled");
#endif
                }    
                
                //display init
               display_init(resolution, colordepth, displaybuffer, displaygamma, displayfilter);

                //rdp init
                rdpq_init();

                //audio init
                audio_init(32000, 4);

                //controller init
                joypad_init();

                //Debug Console Helpers 
                debug_init_isviewer();
                debug_init_usblog();
                debugprint("Libdragon Initilized");
                debugprint("Engine OK"); 
                debugprint("Libdragon OK"); 

                //engine boot hold
                #define ENGINE_BOOT_TICKS 1
                while (tick_get() < ENGINE_BOOT_TICKS) {
                    tick_update();
                    eng_show_init_image();
                }

                eng_show_init_image();

                eng_state = ENG_IDLE;
                break;

            case ENG_IDLE:
                debugprint("ENGINE STATE IDLE"); 
                //hands off to game main.c / entry
                ac_main();
                break;

            case ENG_HOLD:
                debugprint("ENGINE STATE HOLD"); 
                //for console hooks and handling- networking, memory, 64dd swaps, etc.
                //loadscene(998, cutto) //loadscene hook, id 998, cutto transitional data
                break;

            case ENG_DEBUG:
                debugprint("ENGINE STATE DEBUG"); 
                //shortcut to debug scene
                //loadscene(999, cutto) //loadscene hook, id 999, cutto transitional data
                break;

            case ENG_CRASH:
                //when a soft or hard crash is expected, this state is set a frame prior and attempts to log whatever it can before the engine dies 
                debugprint("ENGINE STATE CRASH");
                //logcrashdata()
                while (1) {} /* halt */
                break;
        }
    }

    return 0;
}

//delay of around a second would be here by default as a engine floor
//this step is mean't to intiilized the engine and populate any engine global tables or data- think scenes, dialogue, or entity data
//also checks for what platform and extensions are available- examples include: are we on N64 or Windows 32 bit? 
//if N64, is 64DD plugged in? First boot? if not, has some kind of internet connection data been made prior? 
//not limited to but includes the above. planning on rewriting this to be FSM based-
//states include: INIT, IDLE, HOLD, DEBUG, and CRASH
//
//INIT is the default frame 0 state- if no scenes/data is properly handled , but not a crash or intentional (debug)
//The scene will simply stay here, which should be a black background in theory.
//IDLE is the main gameplay loop , the OK to play state. When this is set , we'd call ac_main or the game's entrypoint.
//HOLD is a reservation state mean't to handle internet configuration, networking functionality, save data and any inter-platform handling.
//DEBUG will go to our debug menu by default as soon as it's called.
//CRASH gets call as close to as a single frame before a hard or soft crash and attempts to log in as many ways as humanely possible- 
//as a .txt document, a .png screenshot, and in the debugging consoles. 
//For some soft crashes, it might look like a softlock, 
//but usually soft crashes will cause the screen to cut to a black screen while hard crashes will hard crash the console on the same frame.
//
//more states may be added at a later date but these drive the main engine itself.

//Global ticks and seconds
//This is used to check the ticks since boot/frame 0. in main.c it's used to check if at least x seconds have passed as a floor.
//If x seconds passed and everything is init, go to ac_main()

static uint64_t s_tick = 0;
static uint64_t s_last = 0;

void tick_update(void) {
    uint64_t s_seconds = get_ticks();
    if (s_last == 0) s_last = s_seconds;
    if (s_seconds - s_last >= TICKS_PER_SECOND) {
        s_last += TICKS_PER_SECOND;
        s_tick++;
    }
}

uint64_t tick_get(void) {
    return s_tick;
}

void tick_reset(void) {
    s_tick = 0;
    s_last = 0;
}

//Dummy.h image
//this shows a image to test 2d rendering- if this stays forever it means it is stuck in init engine state.

void eng_show_init_image(void) { //RGBA16 seems to the default for N64 & Libdragon? 
    surface_t surf = surface_alloc(FMT_RGBA16, DUMMY_WIDTH, DUMMY_HEIGHT); //new surface
    memcpy(surf.buffer, dummy_rgba16, DUMMY_WIDTH * DUMMY_HEIGHT * 2); 

    surface_t *fb = display_get(); //gets the current display
    rdpq_attach(fb, NULL); 
    rdpq_set_mode_copy(false);
    rdpq_tex_blit(&surf,
        (float)((320 - DUMMY_WIDTH)  / 2),
        (float)((240 - DUMMY_HEIGHT) / 2),
        NULL);
    rdpq_detach_show(); //this seems to be the actual "draw image" , not 100% sure

    surface_free(&surf); //cleans up the above image code?
}

