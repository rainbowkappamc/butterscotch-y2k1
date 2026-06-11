#pragma once

/*
 * n64_datawin.h
 * Lightweight data.win scanner and 4 MB chunk streamer for N64.
 *
 * Does NOT parse asset contents — only builds a directory of GMS chunk
 * positions and divides them into 4 MB streaming windows so the runtime
 * can page individual asset groups in and out of a fixed work buffer.
 *
 * Usage:
 *   DataWinIndex* idx = datawin_scan("data.win");   // call during ENG_INIT
 *   datawin_load_window(idx, window_id, buf, BUF_SIZE);
 *   DataWinChunkEntry* e = datawin_find_chunk(idx, "TXTR");
 *   // e->window_id tells you which window to load; e->offset/size for the data
 *   datawin_free(idx);
 */

#include <libdragon.h>
#include <stdint.h>
#include <stdbool.h>

#define DATAWIN_MAX_CHUNKS   64          /* GMS uses ~23 chunk types, 64 is safe  */
#define DATAWIN_CHUNK_LIMIT  (4*1024*1024) /* 4 MB window size                   */

/* ── One GMS chunk in the file ──────────────────────────────────────────── */
typedef struct {
    char     tag[5];        /* null-terminated 4-char tag e.g. "TXTR"         */
    uint32_t file_offset;   /* absolute byte offset of this chunk's header    */
    uint32_t data_size;     /* byte count of chunk payload (excludes 8-byte header) */
    int      window_id;     /* which 4 MB window this chunk belongs to         */
} DataWinChunkEntry;

/* ── One 4 MB streaming window ───────────────────────────────────────────── */
typedef struct {
    uint32_t file_offset;   /* byte offset of first byte of this window        */
    uint32_t byte_length;   /* total bytes spanned (≤ DATAWIN_CHUNK_LIMIT)     */
    int      first_chunk;   /* index into chunks[] of first chunk in window    */
    int      chunk_count;   /* number of chunks in this window                 */
} DataWinWindow;

/* ── Master index ────────────────────────────────────────────────────────── */
typedef struct {
    char               path[128];
    uint32_t           file_size;
    int                chunk_count;
    DataWinChunkEntry  chunks[DATAWIN_MAX_CHUNKS];
    int                window_count;
    DataWinWindow      windows[DATAWIN_MAX_CHUNKS]; /* at most one window per chunk */
} DataWinIndex;

/*
 * datawin_scan — open a data.win from DFS, read all chunk headers,
 * build the chunk directory, divide into 4 MB windows.
 * Returns NULL on error (bad magic, DFS open fail, etc.).
 */
DataWinIndex* datawin_scan(const char* dfs_path);

/*
 * datawin_load_window — read window window_id from disk into buf.
 * buf must be at least DATAWIN_CHUNK_LIMIT bytes (caller allocates).
 * Returns the number of bytes read, or 0 on error.
 */
uint32_t datawin_load_window(DataWinIndex* idx, int window_id,
                              void* buf, uint32_t buf_size);

/*
 * datawin_find_chunk — find a chunk entry by 4-char tag.
 * Returns a pointer into idx->chunks[], or NULL if not found.
 */
DataWinChunkEntry* datawin_find_chunk(DataWinIndex* idx, const char* tag);

/*
 * datawin_free — release the index. Does not touch any loaded window buffer.
 */
void datawin_free(DataWinIndex* idx);

/* ── Name list ───────────────────────────────────────────────────────────── */

typedef struct {
    char** names;   /* heap-allocated array of heap-allocated strings */
    int    count;
} DataWinNameList;

void             datawin_name_list_free(DataWinNameList* list);

/* Parse sound names from the SOND chunk.  Returns NULL if chunk missing. */
DataWinNameList* datawin_get_sounds(DataWinIndex* idx);

/* Parse room names from the ROOM chunk.  Returns NULL if chunk missing. */
DataWinNameList* datawin_get_rooms(DataWinIndex* idx);

/* Return the number of texture pages in the TXTR chunk, or 0. */
int              datawin_get_txtr_count(DataWinIndex* idx);

/* ── Audio entry ─────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t* data;      /* caller must free() */
    uint32_t size;
} DataWinAudoEntry;

/* Load raw audio bytes for AUDO index audo_idx from data.win.
   Returns a heap-allocated DataWinAudoEntry (caller frees .data) or NULL. */
DataWinAudoEntry* datawin_get_audo_entry(DataWinIndex* idx, int audo_idx);

/* Get the AUDO index for a given SOND index.
   Returns -1 if not found or on error. */
int datawin_get_audo_index(DataWinIndex* idx, int sond_idx);
