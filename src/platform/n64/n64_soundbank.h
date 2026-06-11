// n64_soundbank.h
// Stub soundbank — replace entries with your actual asset filenames and indices.
// SFX indices map to N64SoundInstance (fully decoded, LRU cached).
// Music indices map to N64MusicStream (streamed ADPCM).
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// SFX indices
// Used with AudioSystem_play() / AudioSystem_stop() on the SFX instance pool.
// ---------------------------------------------------------------------------
typedef enum {
    SFX_JUMP        = 0,
    SFX_LAND        = 1,
    SFX_COIN        = 2,
    SFX_HURT        = 3,
    SFX_MENU_MOVE   = 4,
    SFX_MENU_SELECT = 5,

    SFX_COUNT       // keep last — used for table bounds
} SfxIndex;

// ---------------------------------------------------------------------------
// Music indices
// Used with AudioSystem_playMusic() / AudioSystem_stopMusic() on the stream pool.
// Up to N64_MAX_MUSIC_STREAMS (4) can play concurrently.
// ---------------------------------------------------------------------------
typedef enum {
    MUS_TITLE    = 0,
    MUS_LEVEL1   = 1,
    MUS_LEVEL2   = 2,
    MUS_GAMEOVER = 3,

    MUS_COUNT    // keep last
} MusicIndex;

// ---------------------------------------------------------------------------
// DFS filename tables
// Wire these into your N64AudioFS open() calls.
// Paths are relative to the DFS root (romfs:/).
// ---------------------------------------------------------------------------
static const char* const SFX_FILENAMES[SFX_COUNT] = {
    "sfx/jump.fm",         // SFX_JUMP
    "sfx/land.fm",         // SFX_LAND
    "sfx/coin.fm",         // SFX_COIN
    "sfx/hurt.fm",         // SFX_HURT
    "sfx/menu_move.fm",    // SFX_MENU_MOVE
    "sfx/menu_select.fm",  // SFX_MENU_SELECT
};

static const char* const MUS_FILENAMES[MUS_COUNT] = {
    "music/title.fm",      // MUS_TITLE
    "music/level1.fm",     // MUS_LEVEL1
    "music/level2.fm",     // MUS_LEVEL2
    "music/gameover.fm",   // MUS_GAMEOVER
};
