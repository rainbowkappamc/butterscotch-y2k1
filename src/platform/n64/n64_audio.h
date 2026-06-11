#pragma once

/*
 * n64_audio.h
 * Shūyatō Engine — N64 Audio System
 *
 * Plays .fm files (IMA ADPCM, big-endian header) via libdragon's audio_push().
 * Structured after Ps2AudioSystem for transparency and cross-platform readability.
 *
 * Two playback paths mirror the PS2 design:
 *   SFX path    — short sounds decoded fully into an LRU PCM cache, played via
 *                 sample-stepping mixer identical to PS2.
 *   Stream path — long music tracks decoded in 16-sample ADPCM blocks on demand,
 *                 double-buffered, fed to audio_push() each frame.
 *
 * .fm format summary (big-endian, 32-byte header + IMA ADPCM):
 *   magic[2]       "FM"
 *   version        0x01
 *   channels       1=mono, 2=stereo
 *   sample_rate    uint16
 *   total_samples  uint32  (per channel)
 *   loop_start     uint32  (0xFFFFFFFF = no loop)
 *   loop_end       uint32  (0xFFFFFFFF = no loop)
 *   flags          uint16  (bit0=HAS_LOOP, bit1=STEREO, bit2=LIVE_UPDATE)
 *   adpcm_size     uint32
 *   loop_pred_L    int16   (ADPCM predictor at loop_start, L/mono)
 *   loop_step_L    uint8   (ADPCM step index at loop_start, L/mono)
 *   loop_pred_R    int16   (R channel; 0 if mono)
 *   loop_step_R    uint8   (R channel; 0 if mono)
 *   reserved       uint16  (must be 0)
 */

#include <libdragon.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Limits ─────────────────────────────────────────────────────────────── */

#define N64_MAX_SFX_INSTANCES    32
#define N64_SFX_LRU_CACHE_SIZE   16
#define N64_MAX_MUSIC_STREAMS     4

#define N64_AUDIO_INSTANCE_ID_BASE  100000
#define N64_NO_LOOP                 0xFFFFFFFFu

/* Mixer output buffer size in stereo frames (must be even).
   libdragon recommends multiples of 16; 256 gives ~6 ms at 44100 Hz.  */
#define N64_MIX_BUFFER_FRAMES    256

/* Streaming: decode this many ADPCM bytes per refill pass.
   16-sample stereo block = 8 bytes per channel; keep a multiple of 16.  */
#define N64_STREAM_ADPCM_CHUNK   4096
#define N64_STREAM_DECODE_SAMPLES (N64_STREAM_ADPCM_CHUNK * 2)

/* .fm flags */
#define FM_FLAG_HAS_LOOP    0x0001
#define FM_FLAG_STEREO      0x0002
#define FM_FLAG_LIVE_UPDATE 0x0004

/* Stereo interleave block size (samples per channel per block) */
#define FM_STEREO_BLOCK      16

/* ── .fm header ─────────────────────────────────────────────────────────── */

typedef struct {
    char     magic[2];        /* "FM"                                         */
    uint8_t  version;         /* 0x01                                         */
    uint8_t  channels;        /* 1 or 2                                       */
    uint16_t sample_rate;
    uint32_t total_samples;   /* per-channel                                  */
    uint32_t loop_start;      /* sample index; N64_NO_LOOP = disabled         */
    uint32_t loop_end;
    uint16_t flags;
    uint32_t adpcm_size;      /* byte count of ADPCM stream after header      */
    int16_t  loop_pred_L;     /* ADPCM predictor at loop_start (L/mono)       */
    uint8_t  loop_step_L;
    int16_t  loop_pred_R;     /* R channel; 0 if mono                         */
    uint8_t  loop_step_R;
    uint16_t reserved;
} FMHeader;   /* all multi-byte fields are big-endian; byte-swapped on load  */

/* ── LRU decoded PCM cache (SFX path) ───────────────────────────────────── */

typedef struct {
    int32_t  fm_index;          /* -1 = free slot                             */
    int16_t* pcm_data;          /* decoded mono or interleaved stereo PCM     */
    uint32_t pcm_sample_count;  /* total mono samples (stereo: L+R pairs)     */
    uint32_t pcm_bytes;
    uint32_t last_access;
} N64DecodedPcmEntry;

/* ── SFX instance ────────────────────────────────────────────────────────── */

typedef struct {
    bool     active;
    int32_t  fm_index;
    int32_t  instance_id;
    int32_t  priority;
    bool     loop;
    bool     paused;

    /* Playback position (32.32 fixed-point for fractional pitch stepping) */
    uint32_t pos_int;
    uint32_t pos_frac;
    uint32_t total_samples;

    float    pitch;         /* runtime pitch                                   */
    float    current_gain;
    float    target_gain;
    float    start_gain;
    float    fade_remaining;
    float    fade_total;
} N64SfxInstance;

/* ── Streaming music instance ────────────────────────────────────────────── */

typedef struct {
    bool     active;
    int32_t  fm_index;
    int32_t  instance_id;
    int32_t  priority;
    bool     loop;
    bool     paused;

    float    pitch;
    float    current_gain;
    float    target_gain;
    float    start_gain;
    float    fade_remaining;
    float    fade_total;

    /* .fm header (retained for loop state) */
    FMHeader header;

    /* DFS streaming state (for .fm files on disk) */
    int      dfs_fd;            /* open DFS handle; -1 = not open             */
    uint32_t adpcm_start;
    uint32_t adpcm_pos;
    uint32_t adpcm_end;

    /* RAM streaming state (for AUDO chunks loaded into RAM) */
    int16_t* ram_pcm;           /* fully decoded mono PCM; NULL if DFS path   */
    uint32_t ram_pcm_samples;   /* total samples                               */
    uint32_t ram_pcm_pos;       /* current read position in samples            */

    /* IMA ADPCM decoder state (L and R channels, persists across chunks)     */
    int32_t  pred_L, pred_R;
    int32_t  step_L, step_R;

    /* Double-buffered decoded PCM (interleaved stereo or mono)               */
    int16_t  buffers[2][N64_STREAM_DECODE_SAMPLES * 2]; /* *2 for stereo      */
    uint32_t buf_samples[2];  /* valid samples in each buffer (mono count)    */
    int      active_buf;
    uint32_t read_pos;        /* sample position within active buffer         */
    bool     needs_refill;
    bool     end_of_track;

    /* Sample counters for loop detection */
    uint32_t sample_pos;      /* current per-channel sample position          */
} N64MusicStream;

/* ── N64 Audio System ────────────────────────────────────────────────────── */

typedef struct {
    /* LRU decoded PCM cache */
    N64DecodedPcmEntry cache[N64_SFX_LRU_CACHE_SIZE];
    uint32_t           cache_counter;

    /* SFX instances */
    N64SfxInstance     instances[N64_MAX_SFX_INSTANCES];
    int32_t            next_instance_id;

    /* Streaming music slots */
    N64MusicStream     streams[N64_MAX_MUSIC_STREAMS];

    /* Mixer output (stereo interleaved) */
    int16_t            mix_buf[N64_MIX_BUFFER_FRAMES * 2];

    float              master_gain;
    bool               initialized;
} N64AudioSystem;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Call once after audio_init().  Returns the system or NULL on failure. */
N64AudioSystem* N64AudioSystem_create(void);

/* Call once per frame from the main loop — mixes active voices and pushes
   to libdragon's audio DMA.                                                  */
void N64AudioSystem_update(N64AudioSystem* sys);

/* Free all resources.  Does not call audio_close(); caller does that. */
void N64AudioSystem_destroy(N64AudioSystem* sys);

/*
 * N64AudioSystem_play — start playback of a .fm file.
 *   fm_index : caller-assigned index (used as sound ID, matches SOND order)
 *   path     : DFS path to the .fm file  e.g. "sounds/snd_coin.fm"
 *   loop     : true = honour FLAG_HAS_LOOP; false = play once regardless
 *   gain     : 0.0–1.0
 *   pitch    : 1.0 = normal
 * Returns an instance ID (>= N64_AUDIO_INSTANCE_ID_BASE) or -1 on failure.
 */
int32_t N64AudioSystem_play(N64AudioSystem* sys, int32_t fm_index,
                             const char* path, bool loop,
                             float gain, float pitch);

void N64AudioSystem_stop(N64AudioSystem* sys, int32_t sound_or_instance);
void N64AudioSystem_stopAll(N64AudioSystem* sys);
bool N64AudioSystem_isPlaying(N64AudioSystem* sys, int32_t sound_or_instance);
void N64AudioSystem_pause(N64AudioSystem* sys, int32_t sound_or_instance);
void N64AudioSystem_resume(N64AudioSystem* sys, int32_t sound_or_instance);
void N64AudioSystem_pauseAll(N64AudioSystem* sys);
void N64AudioSystem_resumeAll(N64AudioSystem* sys);
void N64AudioSystem_setGain(N64AudioSystem* sys, int32_t sound_or_instance,
                             float gain, uint32_t fade_ms);
float N64AudioSystem_getGain(N64AudioSystem* sys, int32_t sound_or_instance);
void  N64AudioSystem_setPitch(N64AudioSystem* sys, int32_t sound_or_instance,
                               float pitch);
float N64AudioSystem_getPitch(N64AudioSystem* sys, int32_t sound_or_instance);
void  N64AudioSystem_setMasterGain(N64AudioSystem* sys, float gain);

/*
 * N64AudioSystem_play_fm — look up name in FM_INDEX, build DFS path, and play.
 * Returns instance ID or -1 if not found / table unavailable.
 */
int32_t N64AudioSystem_play_fm(N64AudioSystem* sys, const char* name,
                                bool loop, float gain, float pitch);

/*
 * N64AudioSystem_play_raw — transcode raw audio bytes (wav/ogg/mp3 from AUDO)
 * to an in-RAM .fm container and start playback.
 * Supports raw PCM WAV (no decode needed) and raw IMA ADPCM WAV.
 * Other formats: extracts PCM via simple header sniff; unknown = skipped.
 * Returns instance ID or -1 on failure.
 */
int32_t N64AudioSystem_play_raw(N64AudioSystem* sys, int32_t fm_index,
                                 const uint8_t* raw, uint32_t raw_size,
                                 bool loop, float gain, float pitch);
