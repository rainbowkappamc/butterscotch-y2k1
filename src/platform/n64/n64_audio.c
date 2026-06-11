/*
 * n64_audio.c
 * Shūyatō Engine — N64 Audio System
 *
 * IMA ADPCM decode + libdragon audio_push() mixer.
 * Structured after Ps2AudioSystem for cross-platform readability.
 */

#include "n64_audio.h"

#include <libdragon.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── IMA ADPCM Tables (identical to PS2 and .fm spec) ───────────────────── */

static const int16_t IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static const int8_t IMA_INDEX_TABLE[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* ── No byte-swap helpers needed ─────────────────────────────────────────
 * N64 (MIPS) is big-endian, identical to the .fm file format.
 * Fields are parsed byte-by-byte (matching fm_n64.c) to avoid any struct
 * padding or alignment ambiguity.
 * ───────────────────────────────────────────────────────────────────────── */

/* ── IMA ADPCM single-nibble decode ─────────────────────────────────────── */

static inline int16_t ima_decode_nibble(uint8_t nibble,
                                         int32_t* pred, int32_t* step_idx) {
    int32_t step  = IMA_STEP_TABLE[*step_idx];
    int32_t delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 1) delta += step >> 2;
    *pred += (nibble & 8) ? -delta : delta;
    if (*pred >  32767) *pred =  32767;
    if (*pred < -32768) *pred = -32768;
    *step_idx += IMA_INDEX_TABLE[nibble & 7];
    if (*step_idx <  0) *step_idx =  0;
    if (*step_idx > 88) *step_idx = 88;
    return (int16_t) *pred;
}

/* ── IMA ADPCM block decode ─────────────────────────────────────────────── */

/* Decodes adpcm_bytes of packed nibble data into out_pcm.
   Updates predictor/step_idx in place (streaming state persists).
   Returns number of samples written.                                         */
static uint32_t ima_decode_block(const uint8_t* adpcm, uint32_t adpcm_bytes,
                                  int16_t* out_pcm, uint32_t max_samples,
                                  int32_t* pred, int32_t* step_idx) {
    uint32_t written = 0;
    for (uint32_t i = 0; i < adpcm_bytes && written < max_samples; i++) {
        uint8_t byte = adpcm[i];
        if (written < max_samples)
            out_pcm[written++] = ima_decode_nibble(byte & 0x0F, pred, step_idx);
        if (written < max_samples)
            out_pcm[written++] = ima_decode_nibble((byte >> 4) & 0x0F, pred, step_idx);
    }
    return written;
}

/* Convenience: full decode with reset state (SFX cache fill path) */
static uint32_t ima_decode_full(const uint8_t* adpcm, uint32_t adpcm_bytes,
                                 int16_t* out_pcm, uint32_t max_samples) {
    int32_t pred = 0, step_idx = 0;
    return ima_decode_block(adpcm, adpcm_bytes, out_pcm, max_samples,
                            &pred, &step_idx);
}

/* ── .fm header load ─────────────────────────────────────────────────────── */

/* Read a .fm header from an open DFS fd positioned at the file start.
 *
 * Parses the 32 raw bytes by explicit offset (matching fm_n64.c) so that
 * struct padding, alignment, and the N64's native big-endian byte order are
 * all irrelevant — no byte-swapping is performed.
 *
 * Byte layout (all multi-byte fields are big-endian, same as N64 native):
 *   0x00  magic[2]       'F','M'
 *   0x02  version        0x01
 *   0x03  channels       1 or 2
 *   0x04  sample_rate    uint16
 *   0x06  total_samples  uint32
 *   0x0A  loop_start     uint32
 *   0x0E  loop_end       uint32
 *   0x12  flags          uint16
 *   0x14  adpcm_size     uint32
 *   0x18  loop_pred_L    int16
 *   0x1A  loop_step_L    uint8
 *   0x1B  loop_pred_R    int16
 *   0x1D  loop_step_R    uint8
 *   0x1E  reserved       uint16 (0)
 *
 * Returns true on success.                                                   */
static bool fm_read_header(int fd, FMHeader* hdr) {
    uint8_t b[32];
    if (dfs_read(b, 1, 32, fd) != 32) return false;

    if (b[0] != 'F' || b[1] != 'M') return false;
    if (b[2] != 0x01)                return false;
    if (b[3] != 1 && b[3] != 2)     return false;

    hdr->magic[0]      = (char)b[0];
    hdr->magic[1]      = (char)b[1];
    hdr->version       = b[2];
    hdr->channels      = b[3];

    hdr->sample_rate   = (uint16_t)( ((uint16_t)b[4]  << 8) | b[5] );

    hdr->total_samples = ((uint32_t)b[6]  << 24) | ((uint32_t)b[7]  << 16)
                       | ((uint32_t)b[8]  <<  8) |  (uint32_t)b[9];

    hdr->loop_start    = ((uint32_t)b[10] << 24) | ((uint32_t)b[11] << 16)
                       | ((uint32_t)b[12] <<  8) |  (uint32_t)b[13];

    hdr->loop_end      = ((uint32_t)b[14] << 24) | ((uint32_t)b[15] << 16)
                       | ((uint32_t)b[16] <<  8) |  (uint32_t)b[17];

    hdr->flags         = (uint16_t)( ((uint16_t)b[18] << 8) | b[19] );

    hdr->adpcm_size    = ((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16)
                       | ((uint32_t)b[22] <<  8) |  (uint32_t)b[23];

    hdr->loop_pred_L   = (int16_t)( ((uint16_t)b[24] << 8) | b[25] );
    hdr->loop_step_L   = b[26];
    hdr->loop_pred_R   = (int16_t)( ((uint16_t)b[27] << 8) | b[28] );
    hdr->loop_step_R   = b[29];
    hdr->reserved      = (uint16_t)( ((uint16_t)b[30] << 8) | b[31] );

    /* Normalise loop fields — if flag is not set, treat as no-loop */
    if (!(hdr->flags & FM_FLAG_HAS_LOOP)) {
        hdr->loop_start = N64_NO_LOOP;
        hdr->loop_end   = N64_NO_LOOP;
    }

    return true;
}

/* ── LRU Decoded PCM Cache ───────────────────────────────────────────────── */

static N64DecodedPcmEntry* cache_get(N64AudioSystem* sys, int32_t fm_index) {
    for (int i = 0; i < N64_SFX_LRU_CACHE_SIZE; i++) {
        if (sys->cache[i].fm_index == fm_index) {
            sys->cache[i].last_access = ++sys->cache_counter;
            return &sys->cache[i];
        }
    }
    return NULL;
}

static bool cache_is_in_use(N64AudioSystem* sys, int32_t fm_index) {
    for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++) {
        if (sys->instances[i].active && sys->instances[i].fm_index == fm_index)
            return true;
    }
    return false;
}

/* Load, decode, and cache an fm file for SFX playback.
   Returns the cache entry or NULL on failure.                                */
static N64DecodedPcmEntry* cache_insert(N64AudioSystem* sys, int32_t fm_index,
                                         const char* path) {
    int fd = dfs_open(path);
    if (fd < 0) {
        debugf("[n64_audio] cache_insert: cannot open %s\n", path);
        return NULL;
    }

    FMHeader hdr;
    if (!fm_read_header(fd, &hdr)) {
        debugf("[n64_audio] cache_insert: bad header %s\n", path);
        dfs_close(fd);
        return NULL;
    }

    /* Read raw ADPCM into a temp buffer */
    uint8_t* adpcm_buf = (uint8_t*) malloc(hdr.adpcm_size);
    if (!adpcm_buf) { dfs_close(fd); return NULL; }
    dfs_read(adpcm_buf, 1, hdr.adpcm_size, fd);
    dfs_close(fd);

    /* Decode: stereo = 2 samples per ADPCM byte per channel */
    uint32_t sample_count  = hdr.total_samples;
    uint32_t pcm_channels  = hdr.channels;
    uint32_t total_pcm     = sample_count * pcm_channels;
    uint32_t pcm_bytes     = total_pcm * sizeof(int16_t);
    int16_t* pcm_data      = (int16_t*) malloc(pcm_bytes);
    if (!pcm_data) { free(adpcm_buf); return NULL; }

    if (hdr.channels == 1) {
        ima_decode_full(adpcm_buf, hdr.adpcm_size, pcm_data, sample_count);
    } else {
        /* Stereo: 16-sample blocks, L then R interleaved in the output */
        uint32_t block_bytes = FM_STEREO_BLOCK / 2; /* 8 bytes per channel per block */
        int32_t pred_L = 0, step_L = 0;
        int32_t pred_R = 0, step_R = 0;
        uint32_t pos   = 0;   /* byte offset in adpcm_buf */
        uint32_t out   = 0;   /* sample index in pcm_data (interleaved) */
        uint32_t done  = 0;   /* per-channel samples decoded */

        while (done < sample_count && pos + block_bytes * 2 <= hdr.adpcm_size) {
            int16_t tmp_L[FM_STEREO_BLOCK];
            int16_t tmp_R[FM_STEREO_BLOCK];
            uint32_t rem = sample_count - done;
            uint32_t n   = rem < FM_STEREO_BLOCK ? rem : FM_STEREO_BLOCK;

            ima_decode_block(adpcm_buf + pos, block_bytes,
                             tmp_L, n, &pred_L, &step_L);
            pos += block_bytes;
            ima_decode_block(adpcm_buf + pos, block_bytes,
                             tmp_R, n, &pred_R, &step_R);
            pos += block_bytes;

            for (uint32_t s = 0; s < n; s++) {
                pcm_data[out++] = tmp_L[s];
                pcm_data[out++] = tmp_R[s];
            }
            done += n;
        }
    }

    free(adpcm_buf);

    /* Resample to 32000 Hz if needed */
    if (hdr.sample_rate != 32000 && sample_count > 0) {
        uint32_t new_count = (uint32_t)((uint64_t)sample_count * 32000 / hdr.sample_rate);
        int16_t* resampled = (int16_t*) malloc(new_count * sizeof(int16_t));
        if (resampled) {
            for (uint32_t i = 0; i < new_count; i++) {
                uint32_t si = (uint32_t)((uint64_t)i * hdr.sample_rate / 32000);
                if (si >= sample_count) si = sample_count - 1;
                resampled[i] = pcm_data[si];
            }
            free(pcm_data);
            pcm_data     = resampled;
            sample_count = new_count;
            pcm_bytes    = new_count * sizeof(int16_t);
        }
    }

    /* Find a free or LRU-evict slot */
    N64DecodedPcmEntry* slot = NULL;
    for (int i = 0; i < N64_SFX_LRU_CACHE_SIZE; i++) {
        if (sys->cache[i].fm_index < 0) { slot = &sys->cache[i]; break; }
    }
    if (!slot) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < N64_SFX_LRU_CACHE_SIZE; i++) {
            if (!cache_is_in_use(sys, sys->cache[i].fm_index)
                && sys->cache[i].last_access < oldest) {
                oldest = sys->cache[i].last_access;
                slot   = &sys->cache[i];
            }
        }
    }
    if (!slot) {
        debugf("[n64_audio] cache full, cannot cache fm_index %ld\n", (long)fm_index);
        free(pcm_data);
        return NULL;
    }

    if (slot->pcm_data) free(slot->pcm_data);
    slot->fm_index         = fm_index;
    slot->pcm_data         = pcm_data;
    slot->pcm_sample_count = sample_count;
    slot->pcm_bytes        = pcm_bytes;
    slot->last_access      = ++sys->cache_counter;
    return slot;
}

/* ── Instance helpers ────────────────────────────────────────────────────── */

static N64SfxInstance* find_sfx_by_id(N64AudioSystem* sys, int32_t id) {
    for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
        if (sys->instances[i].active && sys->instances[i].instance_id == id)
            return &sys->instances[i];
    return NULL;
}

static N64MusicStream* find_stream_by_id(N64AudioSystem* sys, int32_t id) {
    for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++)
        if (sys->streams[i].active && sys->streams[i].instance_id == id)
            return &sys->streams[i];
    return NULL;
}

static bool is_instance_id(int32_t id) {
    return id >= N64_AUDIO_INSTANCE_ID_BASE;
}

/* ── Stream buffer fill ──────────────────────────────────────────────────── */

static void stream_fill_buffer(N64AudioSystem* sys, N64MusicStream* st, int buf_idx) {
    (void)sys;
    int16_t* buf = st->buffers[buf_idx];

    /* ── RAM PCM path (AUDO chunks) ── */
    if (st->ram_pcm) {
        uint32_t remaining = st->ram_pcm_samples - st->ram_pcm_pos;
        uint32_t n = remaining < N64_STREAM_DECODE_SAMPLES
                   ? remaining : N64_STREAM_DECODE_SAMPLES;
        if (n == 0) { st->buf_samples[buf_idx] = 0; st->end_of_track = true; return; }
        memcpy(buf, st->ram_pcm + st->ram_pcm_pos, n * sizeof(int16_t));
        st->ram_pcm_pos += n;
        st->buf_samples[buf_idx] = n;
        if (st->ram_pcm_pos >= st->ram_pcm_samples) st->end_of_track = true;
        return;
    }

    /* ── DFS ADPCM path (.fm files) ── */
    if (st->end_of_track) { st->buf_samples[buf_idx] = 0; return; }

    uint32_t bytes_left = st->adpcm_end - st->adpcm_pos;
    uint32_t chunk      = N64_STREAM_ADPCM_CHUNK;
    if (chunk > bytes_left) chunk = bytes_left;
    if (chunk == 0) { st->buf_samples[buf_idx] = 0; st->end_of_track = true; return; }

    uint8_t* adpcm_tmp = (uint8_t*) malloc(chunk);
    if (!adpcm_tmp) { st->buf_samples[buf_idx] = 0; return; }

    dfs_seek(st->dfs_fd, (int) st->adpcm_pos, SEEK_SET);
    dfs_read(adpcm_tmp, 1, chunk, st->dfs_fd);
    st->adpcm_pos += chunk;

    uint32_t written = 0;
    if (st->header.channels == 1) {
        written = ima_decode_block(adpcm_tmp, chunk, buf,
                                   N64_STREAM_DECODE_SAMPLES,
                                   &st->pred_L, &st->step_L);
    } else {
        uint32_t block_bytes = FM_STEREO_BLOCK / 2;
        uint32_t pos = 0, out = 0;
        while (pos + block_bytes * 2 <= chunk) {
            int16_t tmp_L[FM_STEREO_BLOCK], tmp_R[FM_STEREO_BLOCK];
            uint32_t n = FM_STEREO_BLOCK;
            ima_decode_block(adpcm_tmp + pos, block_bytes,
                             tmp_L, n, &st->pred_L, &st->step_L);
            pos += block_bytes;
            ima_decode_block(adpcm_tmp + pos, block_bytes,
                             tmp_R, n, &st->pred_R, &st->step_R);
            pos += block_bytes;
            for (uint32_t s = 0; s < n; s++) {
                buf[out++] = tmp_L[s];
                buf[out++] = tmp_R[s];
            }
            written += n;
        }
    }

    st->buf_samples[buf_idx] = written;
    if (bytes_left <= chunk) st->end_of_track = true;
    free(adpcm_tmp);
}

/* ── Stream loop seek ────────────────────────────────────────────────────── */

static void stream_seek_to_loop(N64MusicStream* st) {
    FMHeader* h = &st->header;

    /* Restore the predictor state baked in at encode time — no pre-pass */
    st->pred_L = h->loop_pred_L;
    st->step_L = h->loop_step_L;
    st->pred_R = h->loop_pred_R;
    st->step_R = h->loop_step_R;

    /* Seek ADPCM byte position: loop_start sample → byte offset.
       Mono: 2 samples/byte.  Stereo: interleaved 16-sample blocks,
       8 bytes per channel per block.                                         */
    uint32_t loop_sample = h->loop_start;
    uint32_t adpcm_byte_offset;

    if (h->channels == 1) {
        adpcm_byte_offset = loop_sample / 2;
    } else {
        uint32_t full_blocks = loop_sample / FM_STEREO_BLOCK;
        adpcm_byte_offset    = full_blocks * (FM_STEREO_BLOCK / 2) * 2;
    }

    st->adpcm_pos   = st->adpcm_start + adpcm_byte_offset;
    st->sample_pos  = loop_sample;
    st->end_of_track = false;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

N64AudioSystem* N64AudioSystem_create(void) {
    N64AudioSystem* sys = (N64AudioSystem*) calloc(1, sizeof(N64AudioSystem));
    if (!sys) return NULL;

    sys->master_gain     = 1.0f;
    sys->next_instance_id = N64_AUDIO_INSTANCE_ID_BASE;
    sys->initialized     = true;

    for (int i = 0; i < N64_SFX_LRU_CACHE_SIZE; i++)
        sys->cache[i].fm_index = -1;
    for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++)
        sys->streams[i].dfs_fd = -1;

    debugf("[n64_audio] created\n");
    return sys;
}

void N64AudioSystem_destroy(N64AudioSystem* sys) {
    if (!sys) return;
    for (int i = 0; i < N64_SFX_LRU_CACHE_SIZE; i++)
        if (sys->cache[i].pcm_data) free(sys->cache[i].pcm_data);
    for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++) {
        if (sys->streams[i].dfs_fd >= 0) dfs_close(sys->streams[i].dfs_fd);
        if (sys->streams[i].ram_pcm) free(sys->streams[i].ram_pcm);
    }
    free(sys);
}

/* ── Play ────────────────────────────────────────────────────────────────── */

int32_t N64AudioSystem_play(N64AudioSystem* sys, int32_t fm_index,
                             const char* path, bool loop,
                             float gain, float pitch) {
    if (!sys || !path) return -1;

    /* Determine if this is short (SFX) or long (stream).
       Heuristic: open header, check total_samples vs stream threshold.
       Short = total bytes fit in cache entry budget (~512 KB decoded PCM).   */
    int fd = dfs_open(path);
    if (fd < 0) {
        debugf("[n64_audio] play: cannot open %s\n", path);
        return -1;
    }
    FMHeader hdr;
    if (!fm_read_header(fd, &hdr)) {
        dfs_close(fd);
        return -1;
    }

    bool is_stream = false; /* always use SFX cache path */
    (void)is_stream;
    int32_t iid = sys->next_instance_id++;

    dfs_close(fd);

    N64DecodedPcmEntry* entry = cache_get(sys, fm_index);
    if (!entry) entry = cache_insert(sys, fm_index, path);
    if (!entry) return -1;

    N64SfxInstance* inst = NULL;
    for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++) {
        if (!sys->instances[i].active) { inst = &sys->instances[i]; break; }
    }
    if (!inst) {
        debugf("[n64_audio] play: no free SFX slots\n");
        return -1;
    }

    memset(inst, 0, sizeof(N64SfxInstance));
    inst->active        = true;
    inst->fm_index      = fm_index;
    inst->instance_id   = iid;
    inst->loop          = loop && (hdr.flags & FM_FLAG_HAS_LOOP);
    inst->current_gain  = gain;
    inst->target_gain   = gain;
    inst->pitch         = pitch;
    inst->total_samples = entry->pcm_sample_count;
    inst->pos_int       = 0;
    inst->pos_frac      = 0;

    debugf("[n64_audio] play: fm=%ld id=%ld\n", (long)fm_index, (long)iid);
    return iid;
}

/* ── Stop ────────────────────────────────────────────────────────────────── */

void N64AudioSystem_stop(N64AudioSystem* sys, int32_t sound_or_instance) {
    if (!sys) return;
    if (is_instance_id(sound_or_instance)) {
        N64SfxInstance* sfx = find_sfx_by_id(sys, sound_or_instance);
        if (sfx) { sfx->active = false; return; }
        N64MusicStream* st = find_stream_by_id(sys, sound_or_instance);
        if (st) {
            if (st->dfs_fd >= 0) { dfs_close(st->dfs_fd); st->dfs_fd = -1; }
            if (st->ram_pcm)     { free(st->ram_pcm);     st->ram_pcm = NULL; }
            st->active = false;
        }
    } else {
        for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
            if (sys->instances[i].active && sys->instances[i].fm_index == sound_or_instance)
                sys->instances[i].active = false;
        for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++) {
            if (sys->streams[i].active && sys->streams[i].fm_index == sound_or_instance) {
                if (sys->streams[i].dfs_fd >= 0) {
                    dfs_close(sys->streams[i].dfs_fd);
                    sys->streams[i].dfs_fd = -1;
                }
                sys->streams[i].active = false;
            }
        }
    }
}

void N64AudioSystem_stopAll(N64AudioSystem* sys) {
    if (!sys) return;
    for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
        sys->instances[i].active = false;
    for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++) {
        if (sys->streams[i].dfs_fd >= 0) {
            dfs_close(sys->streams[i].dfs_fd);
            sys->streams[i].dfs_fd = -1;
        }
        if (sys->streams[i].ram_pcm) {
            free(sys->streams[i].ram_pcm);
            sys->streams[i].ram_pcm = NULL;
        }
        sys->streams[i].active = false;
    }
}

/* ── Query ───────────────────────────────────────────────────────────────── */

bool N64AudioSystem_isPlaying(N64AudioSystem* sys, int32_t sound_or_instance) {
    if (!sys) return false;
    if (is_instance_id(sound_or_instance)) {
        N64SfxInstance* sfx = find_sfx_by_id(sys, sound_or_instance);
        if (sfx && !sfx->paused) return true;
        N64MusicStream* st = find_stream_by_id(sys, sound_or_instance);
        return (st && !st->paused);
    }
    for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
        if (sys->instances[i].active && sys->instances[i].fm_index == sound_or_instance
            && !sys->instances[i].paused) return true;
    for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++)
        if (sys->streams[i].active && sys->streams[i].fm_index == sound_or_instance
            && !sys->streams[i].paused) return true;
    return false;
}

/* ── Pause / Resume ──────────────────────────────────────────────────────── */

void N64AudioSystem_pause(N64AudioSystem* sys, int32_t sound_or_instance) {
    if (!sys) return;
    if (is_instance_id(sound_or_instance)) {
        N64SfxInstance* sfx = find_sfx_by_id(sys, sound_or_instance);
        if (sfx) { sfx->paused = true; return; }
        N64MusicStream* st = find_stream_by_id(sys, sound_or_instance);
        if (st) st->paused = true;
    } else {
        for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
            if (sys->instances[i].active && sys->instances[i].fm_index == sound_or_instance)
                sys->instances[i].paused = true;
        for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++)
            if (sys->streams[i].active && sys->streams[i].fm_index == sound_or_instance)
                sys->streams[i].paused = true;
    }
}

void N64AudioSystem_resume(N64AudioSystem* sys, int32_t sound_or_instance) {
    if (!sys) return;
    if (is_instance_id(sound_or_instance)) {
        N64SfxInstance* sfx = find_sfx_by_id(sys, sound_or_instance);
        if (sfx) { sfx->paused = false; return; }
        N64MusicStream* st = find_stream_by_id(sys, sound_or_instance);
        if (st) st->paused = false;
    } else {
        for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
            if (sys->instances[i].active && sys->instances[i].fm_index == sound_or_instance)
                sys->instances[i].paused = false;
        for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++)
            if (sys->streams[i].active && sys->streams[i].fm_index == sound_or_instance)
                sys->streams[i].paused = false;
    }
}

void N64AudioSystem_pauseAll(N64AudioSystem* sys) {
    if (!sys) return;
    for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++) sys->instances[i].paused = true;
    for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++) sys->streams[i].paused  = true;
}

void N64AudioSystem_resumeAll(N64AudioSystem* sys) {
    if (!sys) return;
    for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++) sys->instances[i].paused = false;
    for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++) sys->streams[i].paused  = false;
}

/* ── Gain / Pitch ────────────────────────────────────────────────────────── */

void N64AudioSystem_setGain(N64AudioSystem* sys, int32_t sound_or_instance,
                             float gain, uint32_t fade_ms) {
    if (!sys) return;
    /* If fade_ms == 0: instant.  Otherwise set fade ramp. */
    #define APPLY_GAIN(inst) \
        (inst)->target_gain     = gain; \
        (inst)->fade_remaining  = (fade_ms > 0) ? (float)fade_ms / 1000.0f : 0.0f; \
        (inst)->fade_total      = (inst)->fade_remaining; \
        (inst)->start_gain      = (inst)->current_gain; \
        if (fade_ms == 0) (inst)->current_gain = gain;

    if (is_instance_id(sound_or_instance)) {
        N64SfxInstance* sfx = find_sfx_by_id(sys, sound_or_instance);
        if (sfx) { APPLY_GAIN(sfx); return; }
        N64MusicStream* st = find_stream_by_id(sys, sound_or_instance);
        if (st)  { APPLY_GAIN(st);  return; }
    } else {
        for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
            if (sys->instances[i].active && sys->instances[i].fm_index == sound_or_instance)
                { APPLY_GAIN(&sys->instances[i]); }
        for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++)
            if (sys->streams[i].active && sys->streams[i].fm_index == sound_or_instance)
                { APPLY_GAIN(&sys->streams[i]); }
    }
    #undef APPLY_GAIN
}

float N64AudioSystem_getGain(N64AudioSystem* sys, int32_t sound_or_instance) {
    if (!sys) return 0.0f;
    if (is_instance_id(sound_or_instance)) {
        N64SfxInstance* sfx = find_sfx_by_id(sys, sound_or_instance);
        if (sfx) return sfx->current_gain;
        N64MusicStream* st = find_stream_by_id(sys, sound_or_instance);
        if (st)  return st->current_gain;
    } else {
        for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
            if (sys->instances[i].active && sys->instances[i].fm_index == sound_or_instance)
                return sys->instances[i].current_gain;
        for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++)
            if (sys->streams[i].active && sys->streams[i].fm_index == sound_or_instance)
                return sys->streams[i].current_gain;
    }
    return 0.0f;
}

void N64AudioSystem_setPitch(N64AudioSystem* sys, int32_t sound_or_instance, float pitch) {
    if (!sys) return;
    if (is_instance_id(sound_or_instance)) {
        N64SfxInstance* sfx = find_sfx_by_id(sys, sound_or_instance);
        if (sfx) { sfx->pitch = pitch; return; }
        N64MusicStream* st = find_stream_by_id(sys, sound_or_instance);
        if (st)  { st->pitch  = pitch; return; }
    } else {
        for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++)
            if (sys->instances[i].active && sys->instances[i].fm_index == sound_or_instance)
                sys->instances[i].pitch = pitch;
        for (int i = 0; i < N64_MAX_MUSIC_STREAMS; i++)
            if (sys->streams[i].active && sys->streams[i].fm_index == sound_or_instance)
                sys->streams[i].pitch = pitch;
    }
}

float N64AudioSystem_getPitch(N64AudioSystem* sys, int32_t sound_or_instance) {
    if (!sys) return 1.0f;
    if (is_instance_id(sound_or_instance)) {
        N64SfxInstance* sfx = find_sfx_by_id(sys, sound_or_instance);
        if (sfx) return sfx->pitch;
        N64MusicStream* st = find_stream_by_id(sys, sound_or_instance);
        if (st)  return st->pitch;
    }
    return 1.0f;
}

void N64AudioSystem_setMasterGain(N64AudioSystem* sys, float gain) {
    if (sys) sys->master_gain = gain;
}

/* ── Update (call once per frame) ───────────────────────────────────────── */

void N64AudioSystem_update(N64AudioSystem* sys) {
    if (!sys || !sys->initialized) return;

    /* How many stereo frames does libdragon want right now? */
    int needed = audio_get_buffer_length();
    if (needed <= 0) return;
    if (needed > N64_MIX_BUFFER_FRAMES) needed = N64_MIX_BUFFER_FRAMES;

    /* dt for fade calculations (approximate 1/60 s per frame) */
    float dt = 1.0f / 60.0f;

    /* Zero the mix buffer */
    memset(sys->mix_buf, 0, (size_t)needed * 2 * sizeof(int16_t));

    /* ── SFX instances ── */
    for (int ii = 0; ii < N64_MAX_SFX_INSTANCES; ii++) {
        N64SfxInstance* inst = &sys->instances[ii];
        if (!inst->active || inst->paused) continue;

        N64DecodedPcmEntry* entry = cache_get(sys, inst->fm_index);
        if (!entry) { inst->active = false; continue; }

        /* Advance gain fade */
        if (inst->fade_remaining > 0.0f) {
            inst->fade_remaining -= dt;
            if (inst->fade_remaining <= 0.0f) {
                inst->fade_remaining = 0.0f;
                inst->current_gain   = inst->target_gain;
            } else {
                float t = 1.0f - (inst->fade_remaining / inst->fade_total);
                inst->current_gain = inst->start_gain +
                                     (inst->target_gain - inst->start_gain) * t;
            }
        }

        float eff_gain = inst->current_gain * sys->master_gain;

        /* Mix samples with 32.32 fixed-point pitch stepping */
        uint32_t step_int  = (uint32_t) inst->pitch;
        uint32_t step_frac = (uint32_t)((inst->pitch - (float)step_int) * (float)0xFFFFFFFFu);
        bool is_stereo = (entry->pcm_sample_count * 2 == entry->pcm_bytes / sizeof(int16_t));
        (void)is_stereo; /* used implicitly via channel stride */

        for (int s = 0; s < needed; s++) {
            if (inst->pos_int >= inst->total_samples) {
                if (inst->loop) {
                    inst->pos_int  = 0;
                    inst->pos_frac = 0;
                } else {
                    inst->active = false;
                    break;
                }
            }

            int16_t samp_L = entry->pcm_data[inst->pos_int];
            int16_t samp_R = samp_L; /* mono: both channels same */

            int32_t out_L = sys->mix_buf[s * 2 + 0] + (int32_t)(samp_L * eff_gain);
            int32_t out_R = sys->mix_buf[s * 2 + 1] + (int32_t)(samp_R * eff_gain);
            if (out_L >  32767) out_L =  32767;
            if (out_L < -32768) out_L = -32768;
            if (out_R >  32767) out_R =  32767;
            if (out_R < -32768) out_R = -32768;
            sys->mix_buf[s * 2 + 0] = (int16_t) out_L;
            sys->mix_buf[s * 2 + 1] = (int16_t) out_R;

            /* Advance position with fractional pitch */
            inst->pos_frac += step_frac;
            inst->pos_int  += step_int + (inst->pos_frac < step_frac ? 1 : 0);
        }
    }

    /* ── Music streams ── */
    for (int si = 0; si < N64_MAX_MUSIC_STREAMS; si++) {
        N64MusicStream* st = &sys->streams[si];
        if (!st->active || st->paused) continue;

        /* Advance gain fade */
        if (st->fade_remaining > 0.0f) {
            st->fade_remaining -= dt;
            if (st->fade_remaining <= 0.0f) {
                st->fade_remaining = 0.0f;
                st->current_gain   = st->target_gain;
            } else {
                float t = 1.0f - (st->fade_remaining / st->fade_total);
                st->current_gain = st->start_gain +
                                   (st->target_gain - st->start_gain) * t;
            }
        }

        float eff_gain = st->current_gain * sys->master_gain;
        bool  stereo   = (st->header.channels == 2);

        for (int s = 0; s < needed; s++) {
            /* Swap buffers when active is exhausted */
            if (st->read_pos >= st->buf_samples[st->active_buf]) {
                if (st->needs_refill) {
                    stream_fill_buffer(sys, st, st->active_buf);
                    st->needs_refill = false;
                }
                int next = st->active_buf ^ 1;
                if (st->buf_samples[next] == 0) {
                    /* End of track */
                    if (st->loop && (st->header.flags & FM_FLAG_HAS_LOOP)) {
                        stream_seek_to_loop(st);
                        stream_fill_buffer(sys, st, 0);
                        stream_fill_buffer(sys, st, 1);
                        st->active_buf = 0;
                        st->read_pos   = 0;
                        st->needs_refill = false;
                    } else {
                        if (st->dfs_fd >= 0) { dfs_close(st->dfs_fd); st->dfs_fd = -1; }
                        st->active = false;
                        break;
                    }
                } else {
                    st->active_buf = next;
                    st->read_pos   = 0;
                    st->needs_refill = true;
                }
            }

            int16_t samp_L, samp_R;
            if (stereo) {
                samp_L = st->buffers[st->active_buf][st->read_pos * 2 + 0];
                samp_R = st->buffers[st->active_buf][st->read_pos * 2 + 1];
            } else {
                samp_L = samp_R = st->buffers[st->active_buf][st->read_pos];
            }
            st->read_pos++;
            st->sample_pos++;

            /* Loop point check */
            if (st->loop && (st->header.flags & FM_FLAG_HAS_LOOP)
                && st->sample_pos >= st->header.loop_end) {
                stream_seek_to_loop(st);
                stream_fill_buffer(sys, st, 0);
                stream_fill_buffer(sys, st, 1);
                st->active_buf   = 0;
                st->read_pos     = 0;
                st->needs_refill = false;
            }

            int32_t out_L = sys->mix_buf[s * 2 + 0] + (int32_t)(samp_L * eff_gain);
            int32_t out_R = sys->mix_buf[s * 2 + 1] + (int32_t)(samp_R * eff_gain);
            if (out_L >  32767) out_L =  32767;
            if (out_L < -32768) out_L = -32768;
            if (out_R >  32767) out_R =  32767;
            if (out_R < -32768) out_R = -32768;
            sys->mix_buf[s * 2 + 0] = (int16_t) out_L;
            sys->mix_buf[s * 2 + 1] = (int16_t) out_R;
        }
    }

    /* Push mixed output to libdragon's audio DMA */
    audio_push(sys->mix_buf, needed, true /* wait if full */);
}

/* ── N64AudioSystem_play_fm ──────────────────────────────────────────────── */

#if __has_include("fm_contenttable.h")
#include "fm_contenttable.h"
#define FM_AUDIO_AVAILABLE 1
#else
#define FM_AUDIO_AVAILABLE 0
#define FM_INDEX_COUNT 0
#endif

int32_t N64AudioSystem_play_fm(N64AudioSystem* sys, const char* name,
                                bool loop, float gain, float pitch) {
    if (!sys || !name) return -1;

#if FM_AUDIO_AVAILABLE
    for (int i = 0; i < FM_INDEX_COUNT; i++) {
        if (strcmp(FM_INDEX[i].name, name) == 0) {
            char path[128];
            snprintf(path, sizeof(path),
                     "contents/audio/fm/%s.fm", name);
            return N64AudioSystem_play(sys, i, path, loop, gain, pitch);
        }
    }
    debugf("[n64_audio] play_fm: '%s' not in FM_INDEX\n", name);
#else
    (void)loop; (void)gain; (void)pitch;
    debugf("[n64_audio] play_fm: FM table unavailable\n");
#endif
    return -1;
}

/* ── WAV header sniff ────────────────────────────────────────────────────── */

/* Returns true if the buffer looks like a RIFF WAV and fills out the fields.
   Supports PCM (format 1) and IMA ADPCM (format 17 / 0x11).
   data_offset is set to the byte offset of raw sample data within raw[].    */
static bool wav_sniff(const uint8_t* raw, uint32_t raw_size,
                      uint16_t* out_channels, uint32_t* out_rate,
                      uint16_t* out_format, uint32_t* out_data_offset,
                      uint32_t* out_data_size) {
    if (raw_size < 44) return false;
    if (raw[0]!='R'||raw[1]!='I'||raw[2]!='F'||raw[3]!='F') return false;
    if (raw[8]!='W'||raw[9]!='A'||raw[10]!='V'||raw[11]!='E') return false;

    /* Walk chunks to find fmt and data */
    uint32_t pos = 12;
    uint16_t fmt_tag = 0, channels = 0;
    uint32_t rate = 0;
    bool found_fmt = false, found_data = false;

    while (pos + 8 <= raw_size) {
        uint32_t chunk_size = (uint32_t)raw[pos+4]
                            | ((uint32_t)raw[pos+5] << 8)
                            | ((uint32_t)raw[pos+6] << 16)
                            | ((uint32_t)raw[pos+7] << 24);

        if (raw[pos]=='f'&&raw[pos+1]=='m'&&raw[pos+2]=='t'&&raw[pos+3]==' ') {
            if (pos + 8 + 16 > raw_size) break;
            fmt_tag  = (uint16_t)(raw[pos+8]  | (raw[pos+9]  << 8));
            channels = (uint16_t)(raw[pos+10] | (raw[pos+11] << 8));
            rate     = (uint32_t)(raw[pos+12] | (raw[pos+13]<<8)
                                | (raw[pos+14]<<16) | (raw[pos+15]<<24));
            found_fmt = true;
        } else if (raw[pos]=='d'&&raw[pos+1]=='a'&&raw[pos+2]=='t'&&raw[pos+3]=='a') {
            *out_data_offset = pos + 8;
            *out_data_size   = chunk_size;
            found_data = true;
        }

        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++; /* RIFF word-align */
    }

    if (!found_fmt || !found_data) return false;
    *out_channels = channels;
    *out_rate     = rate;
    *out_format   = fmt_tag;
    return true;
}

/* ── N64AudioSystem_play_raw ─────────────────────────────────────────────── */

int32_t N64AudioSystem_play_raw(N64AudioSystem* sys, int32_t fm_index,
                                 const uint8_t* raw, uint32_t raw_size,
                                 bool loop, float gain, float pitch) {
    if (!sys || !raw || raw_size == 0) return -1;

    uint16_t wav_channels = 1, wav_format = 0;
    uint32_t wav_rate = 22050, wav_data_off = 0, wav_data_size = 0;

    /* Sniff for WAV */
    if (!wav_sniff(raw, raw_size, &wav_channels, &wav_rate,
                   &wav_format, &wav_data_off, &wav_data_size)) {
        debugf("[n64_audio] play_raw: unsupported format for fm_index %ld\n",
               (long)fm_index);
        return -1;
    }

    const uint8_t* audio_data = raw + wav_data_off;
    uint32_t       audio_size = wav_data_size;
    uint32_t       n_samples  = 0;

    /* Mix to mono s16 — that's all we need for the mixer */
    int16_t* pcm_cache = NULL;

    if (wav_format == 1) {
        /* PCM s16 LE — swap bytes to BE, mix to mono if needed */
        n_samples  = audio_size / (2 * wav_channels);
        pcm_cache  = (int16_t*) malloc(n_samples * sizeof(int16_t));
        if (!pcm_cache) return -1;
        const uint8_t* src = audio_data;
        if (wav_channels == 1) {
            for (uint32_t s = 0; s < n_samples; s++) {
                pcm_cache[s] = (int16_t)(src[s*2] | (src[s*2+1] << 8));
            }
        } else {
            for (uint32_t s = 0; s < n_samples; s++) {
                int32_t sum = 0;
                for (uint16_t c = 0; c < wav_channels; c++) {
                    uint32_t off = (s * wav_channels + c) * 2;
                    sum += (int16_t)(src[off] | (src[off+1] << 8));
                }
                pcm_cache[s] = (int16_t)(sum / wav_channels);
            }
        }
    } else if (wav_format == 0x11 || wav_format == 17) {
        /* IMA ADPCM WAV — decode then mix to mono */
        uint32_t max_s = audio_size * 2 * wav_channels;
        int16_t* tmp   = (int16_t*) malloc(max_s * sizeof(int16_t));
        if (!tmp) return -1;
        uint32_t got = ima_decode_full(audio_data, audio_size, tmp, max_s);
        n_samples     = got / wav_channels;
        pcm_cache     = (int16_t*) malloc(n_samples * sizeof(int16_t));
        if (!pcm_cache) { free(tmp); return -1; }
        if (wav_channels == 1) {
            memcpy(pcm_cache, tmp, n_samples * sizeof(int16_t));
        } else {
            for (uint32_t s = 0; s < n_samples; s++) {
                int32_t sum = 0;
                for (uint16_t c = 0; c < wav_channels; c++) sum += tmp[s * wav_channels + c];
                pcm_cache[s] = (int16_t)(sum / wav_channels);
            }
        }
        free(tmp);
    } else {
        debugf("[n64_audio] play_raw: unsupported WAV format 0x%04X\n", wav_format);
        return -1;
    }

    /* Resample to 32000 Hz */
    if (wav_rate != 32000 && n_samples > 0) {
        uint32_t new_n     = (uint32_t)((uint64_t)n_samples * 32000 / wav_rate);
        int16_t* resampled = (int16_t*) malloc(new_n * sizeof(int16_t));
        if (resampled) {
            for (uint32_t i = 0; i < new_n; i++) {
                uint32_t si = (uint32_t)((uint64_t)i * wav_rate / 32000);
                if (si >= n_samples) si = n_samples - 1;
                resampled[i] = pcm_cache[si];
            }
            free(pcm_cache);
            pcm_cache = resampled;
            n_samples = new_n;
        }
    }

    int32_t iid = sys->next_instance_id++;

    /* Everything goes through the SFX cache path */
    N64DecodedPcmEntry* slot = NULL;
    for (int i = 0; i < N64_SFX_LRU_CACHE_SIZE; i++) {
        if (sys->cache[i].fm_index < 0) { slot = &sys->cache[i]; break; }
    }
    if (!slot) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < N64_SFX_LRU_CACHE_SIZE; i++) {
            if (!cache_is_in_use(sys, sys->cache[i].fm_index)
                && sys->cache[i].last_access < oldest) {
                oldest = sys->cache[i].last_access;
                slot   = &sys->cache[i];
            }
        }
    }
    if (!slot) { free(pcm_cache); return -1; }
    if (slot->pcm_data) free(slot->pcm_data);
    slot->fm_index         = fm_index;
    slot->pcm_data         = pcm_cache;
    slot->pcm_sample_count = n_samples;
    slot->pcm_bytes        = n_samples * sizeof(int16_t);
    slot->last_access      = ++sys->cache_counter;

    N64SfxInstance* inst = NULL;
    for (int i = 0; i < N64_MAX_SFX_INSTANCES; i++) {
        if (!sys->instances[i].active) { inst = &sys->instances[i]; break; }
    }
    if (!inst) return -1;

    memset(inst, 0, sizeof(N64SfxInstance));
    inst->active        = true;
    inst->fm_index      = fm_index;
    inst->instance_id   = iid;
    inst->loop          = loop;
    inst->current_gain  = gain;
    inst->target_gain   = gain;
    inst->pitch         = pitch;
    inst->total_samples = n_samples;

    debugf("[n64_audio] play_raw sfx: fm=%ld id=%ld samples=%lu\n",
           (long)fm_index, (long)iid, (unsigned long)n_samples);
    return iid;
}
