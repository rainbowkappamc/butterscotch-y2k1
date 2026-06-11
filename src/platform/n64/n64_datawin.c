/*
 * n64_datawin.c
 * Lightweight data.win chunk scanner and 4 MB window streamer for N64.
 */

#include "n64_datawin.h"
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define debugprint(msg) debugf("[n64_datawin] %s\n", msg)

/* ── Little-endian helpers (data.win is LE, N64 is BE) ───────────────────── */

static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ── datawin_scan ─────────────────────────────────────────────────────────── */

DataWinIndex* datawin_scan(const char* dfs_path) {
    int fd = dfs_open(dfs_path);
    if (fd < 0) {
        debugf("[n64_datawin] failed to open: %s\n", dfs_path);
        return NULL;
    }

    int file_size = dfs_size(fd);

    /* Read and validate FORM header (8 bytes) */
    uint8_t hdr[8];
    if (dfs_read(hdr, 1, 8, fd) != 8) {
        debugprint("short read on FORM header");
        dfs_close(fd);
        return NULL;
    }
    if (hdr[0] != 'F' || hdr[1] != 'O' || hdr[2] != 'R' || hdr[3] != 'M') {
        debugprint("bad magic — not a data.win file");
        dfs_close(fd);
        return NULL;
    }

    DataWinIndex* idx = (DataWinIndex*) calloc(1, sizeof(DataWinIndex));
    strncpy(idx->path, dfs_path, sizeof(idx->path) - 1);
    idx->file_size   = (uint32_t) file_size;
    idx->chunk_count = 0;
    idx->window_count = 0;

    /* ── Scan chunk headers ─────────────────────────────────────────────── */
    uint32_t pos = 8; /* byte offset right after FORM header */

    while ((int)pos < file_size && idx->chunk_count < DATAWIN_MAX_CHUNKS) {
        uint8_t chunk_hdr[8];
        dfs_seek(fd, (int)pos, SEEK_SET);
        if (dfs_read(chunk_hdr, 1, 8, fd) != 8) break;

        /* chunk_hdr[0..3] = tag, chunk_hdr[4..7] = LE payload size */
        DataWinChunkEntry* e = &idx->chunks[idx->chunk_count];
        e->tag[0]    = (char) chunk_hdr[0];
        e->tag[1]    = (char) chunk_hdr[1];
        e->tag[2]    = (char) chunk_hdr[2];
        e->tag[3]    = (char) chunk_hdr[3];
        e->tag[4]    = '\0';
        e->file_offset = pos;
        e->data_size   = le32(chunk_hdr + 4);
        e->window_id   = -1; /* assigned below */

        debugf("[n64_datawin] chunk [%d] %s @ 0x%08lX size=%lu\n",
               idx->chunk_count, e->tag, (unsigned long)e->file_offset, (unsigned long)e->data_size);

        idx->chunk_count++;
        pos += 8 + e->data_size; /* advance past header + payload */
    }

    dfs_close(fd);
    debugf("[n64_datawin] scanned %d chunks from %s\n", idx->chunk_count, dfs_path);

    /* ── Divide chunks into 4 MB windows ────────────────────────────────── */
    /*
     * Greedy bin-packing: accumulate chunks into the current window until
     * adding the next chunk would push total bytes over DATAWIN_CHUNK_LIMIT.
     * Each window boundary must fall on a chunk header boundary.
     */
    int   wi          = 0;       /* current window index          */
    uint32_t w_bytes  = 0;       /* bytes accumulated this window */
    int   w_first     = 0;       /* first chunk index this window */

    for (int ci = 0; ci < idx->chunk_count; ci++) {
        DataWinChunkEntry* e = &idx->chunks[ci];
        uint32_t chunk_total = 8 + e->data_size; /* header + payload */

        /* If this chunk alone exceeds limit, it still gets its own window */
        if (w_bytes > 0 && w_bytes + chunk_total > DATAWIN_CHUNK_LIMIT) {
            /* Close current window */
            DataWinWindow* w = &idx->windows[wi];
            w->file_offset = idx->chunks[w_first].file_offset;
            w->byte_length = w_bytes;
            w->first_chunk = w_first;
            w->chunk_count = ci - w_first;
            wi++;
            w_bytes = 0;
            w_first = ci;
        }

        e->window_id = wi;
        w_bytes += chunk_total;
    }

    /* Close final window */
    if (idx->chunk_count > 0 && w_bytes > 0) {
        DataWinWindow* w = &idx->windows[wi];
        w->file_offset = idx->chunks[w_first].file_offset;
        w->byte_length = w_bytes;
        w->first_chunk = w_first;
        w->chunk_count = idx->chunk_count - w_first;
        wi++;
    }

    idx->window_count = wi;

    for (int w = 0; w < idx->window_count; w++) {
        debugf("[n64_datawin] window [%d] @ 0x%08lX  %lu bytes  %d chunks\n",
               w,
               (unsigned long)idx->windows[w].file_offset,
               (unsigned long)idx->windows[w].byte_length,
               idx->windows[w].chunk_count);
    }

    debugf("[n64_datawin] %d windows total\n", idx->window_count);
    return idx;
}

/* ── datawin_load_window ──────────────────────────────────────────────────── */

uint32_t datawin_load_window(DataWinIndex* idx, int window_id,
                              void* buf, uint32_t buf_size) {
    if (!idx || !buf || window_id < 0 || window_id >= idx->window_count)
        return 0;

    DataWinWindow* w = &idx->windows[window_id];
    if (w->byte_length > buf_size) {
        debugf("[n64_datawin] window %d is %lu bytes, buffer is only %lu\n",
               window_id, (unsigned long)w->byte_length, (unsigned long)buf_size);
        return 0;
    }

    int fd = dfs_open(idx->path);
    if (fd < 0) {
        debugf("[n64_datawin] failed to reopen %s\n", idx->path);
        return 0;
    }

    dfs_seek(fd, (int)w->file_offset, SEEK_SET);
    int read = dfs_read(buf, 1, (int)w->byte_length, fd);
    dfs_close(fd);

    debugf("[n64_datawin] loaded window %d: %d bytes\n", window_id, read);
    return (uint32_t) read;
}

/* ── datawin_find_chunk ───────────────────────────────────────────────────── */

DataWinChunkEntry* datawin_find_chunk(DataWinIndex* idx, const char* tag) {
    if (!idx || !tag) return NULL;
    for (int i = 0; i < idx->chunk_count; i++) {
        if (strncmp(idx->chunks[i].tag, tag, 4) == 0)
            return &idx->chunks[i];
    }
    return NULL;
}

/* ── datawin_free ─────────────────────────────────────────────────────────── */

void datawin_free(DataWinIndex* idx) {
    free(idx);
}

/* ── String reader ────────────────────────────────────────────────────────── */
/* data.win strings: uint32 LE length + characters + null terminator.
   abs_offset is the absolute file position of the length prefix.            */

static char* read_string_at(int fd, uint32_t abs_offset) {
    if (abs_offset == 0) return NULL;
    /* In data.win, string pointers point directly to the character bytes.
       The uint32 length prefix lives at abs_offset - 4.
       Read it to know how many bytes to pull, then read the chars.          */
    dfs_seek(fd, (int)(abs_offset - 4), SEEK_SET);
    uint8_t len_buf[4];
    if (dfs_read(len_buf, 1, 4, fd) != 4) return NULL;
    uint32_t len = le32(len_buf);
    if (len == 0 || len > 1024) return NULL;
    char* str = (char*) malloc(len + 1);
    if (!str) return NULL;
    dfs_read(str, 1, len, fd); /* fd is now positioned at abs_offset */
    str[len] = '\0';
    return str;
}

/* ── Pointer table reader ────────────────────────────────────────────────── */
/* Reads count + count*uint32 pointer table starting at current fd position. */

static uint32_t* read_ptr_table(int fd, uint32_t* out_count) {
    uint8_t buf[4];
    if (dfs_read(buf, 1, 4, fd) != 4) { *out_count = 0; return NULL; }
    *out_count = le32(buf);
    if (*out_count == 0) return NULL;
    uint32_t* ptrs = (uint32_t*) malloc(*out_count * sizeof(uint32_t));
    for (uint32_t i = 0; i < *out_count; i++) {
        uint8_t p[4];
        dfs_read(p, 1, 4, fd);
        ptrs[i] = le32(p);
    }
    return ptrs;
}

/* ── datawin_name_list_free ───────────────────────────────────────────────── */

void datawin_name_list_free(DataWinNameList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) free(list->names[i]);
    free(list->names);
    free(list);
}

/* ── datawin_get_sounds ───────────────────────────────────────────────────── */

DataWinNameList* datawin_get_sounds(DataWinIndex* idx) {
    DataWinChunkEntry* e = datawin_find_chunk(idx, "SOND");
    if (!e) return NULL;

    int fd = dfs_open(idx->path);
    if (fd < 0) return NULL;

    /* Seek past 8-byte chunk header to payload */
    dfs_seek(fd, (int)(e->file_offset + 8), SEEK_SET);

    uint32_t count;
    uint32_t* ptrs = read_ptr_table(fd, &count);
    if (!ptrs || count == 0) { dfs_close(fd); free(ptrs); return NULL; }

    DataWinNameList* list = (DataWinNameList*) malloc(sizeof(DataWinNameList));
    list->count = (int) count;
    list->names = (char**) malloc(count * sizeof(char*));

    for (uint32_t i = 0; i < count; i++) {
        /* Seek to sound entry — first field is name string pointer (uint32 LE abs offset) */
        dfs_seek(fd, (int) ptrs[i], SEEK_SET);
        uint8_t name_ptr_buf[4];
        dfs_read(name_ptr_buf, 1, 4, fd);
        uint32_t name_ptr = le32(name_ptr_buf);
        list->names[i] = read_string_at(fd, name_ptr);
        if (!list->names[i]) list->names[i] = strdup("?");
    }

    free(ptrs);
    dfs_close(fd);
    debugf("[n64_datawin] got %d sounds\n", list->count);
    return list;
}

/* ── datawin_get_rooms ────────────────────────────────────────────────────── */

DataWinNameList* datawin_get_rooms(DataWinIndex* idx) {
    DataWinChunkEntry* e = datawin_find_chunk(idx, "ROOM");
    if (!e) return NULL;

    int fd = dfs_open(idx->path);
    if (fd < 0) return NULL;

    dfs_seek(fd, (int)(e->file_offset + 8), SEEK_SET);

    uint32_t count;
    uint32_t* ptrs = read_ptr_table(fd, &count);
    if (!ptrs || count == 0) { dfs_close(fd); free(ptrs); return NULL; }

    DataWinNameList* list = (DataWinNameList*) malloc(sizeof(DataWinNameList));
    list->count = (int) count;
    list->names = (char**) malloc(count * sizeof(char*));

    for (uint32_t i = 0; i < count; i++) {
        /* Room entry — first field is name string pointer */
        dfs_seek(fd, (int) ptrs[i], SEEK_SET);
        uint8_t name_ptr_buf[4];
        dfs_read(name_ptr_buf, 1, 4, fd);
        uint32_t name_ptr = le32(name_ptr_buf);
        list->names[i] = read_string_at(fd, name_ptr);
        if (!list->names[i]) list->names[i] = strdup("?");
    }

    free(ptrs);
    dfs_close(fd);
    debugf("[n64_datawin] got %d rooms\n", list->count);
    return list;
}

/* ── datawin_get_audo_index ───────────────────────────────────────────────── */

int datawin_get_audo_index(DataWinIndex* idx, int sond_idx) {
    DataWinChunkEntry* e = datawin_find_chunk(idx, "SOND");
    if (!e) return -1;

    int fd = dfs_open(idx->path);
    if (fd < 0) return -1;

    /* Pointer table: count + count*uint32 pointers */
    dfs_seek(fd, (int)(e->file_offset + 8), SEEK_SET);
    uint32_t count;
    uint32_t* ptrs = read_ptr_table(fd, &count);
    if (!ptrs || (uint32_t)sond_idx >= count) {
        free(ptrs); dfs_close(fd); return -1;
    }

    /* Seek to SOND entry; audioFile is at offset +36 (9 x uint32 after name ptr):
       name(4) flags(4) type(4) file(4) effects(4) volume(4) pitch(4) audioGroup(4) audioFile(4)
       = 8 fields before audioFile, but name/type/file are string ptrs (4 bytes each).
       Layout: name_ptr(4) flags(4) type_ptr(4) file_ptr(4) effects(4)
               volume(4) pitch(4) audioGroup(4) audioFile(4) = offset 32 from entry start */
    dfs_seek(fd, (int)(ptrs[sond_idx] + 32), SEEK_SET);
    uint8_t buf[4];
    dfs_read(buf, 1, 4, fd);
    int audo_idx = (int) le32(buf);

    free(ptrs);
    dfs_close(fd);
    return audo_idx;
}

/* ── datawin_get_audo_entry ───────────────────────────────────────────────── */

DataWinAudoEntry* datawin_get_audo_entry(DataWinIndex* idx, int audo_idx) {
    DataWinChunkEntry* e = datawin_find_chunk(idx, "AUDO");
    if (!e) return NULL;

    int fd = dfs_open(idx->path);
    if (fd < 0) return NULL;

    /* AUDO pointer table */
    dfs_seek(fd, (int)(e->file_offset + 8), SEEK_SET);
    uint32_t count;
    uint32_t* ptrs = read_ptr_table(fd, &count);
    if (!ptrs || (uint32_t)audo_idx >= count) {
        free(ptrs); dfs_close(fd); return NULL;
    }

    /* Each entry: uint32 dataSize, then dataSize bytes */
    dfs_seek(fd, (int) ptrs[audo_idx], SEEK_SET);
    uint8_t sz_buf[4];
    dfs_read(sz_buf, 1, 4, fd);
    uint32_t data_size = le32(sz_buf);

    if (data_size == 0) { free(ptrs); dfs_close(fd); return NULL; }

    uint8_t* data = (uint8_t*) malloc(data_size);
    if (!data) { free(ptrs); dfs_close(fd); return NULL; }
    dfs_read(data, 1, data_size, fd);

    free(ptrs);
    dfs_close(fd);

    DataWinAudoEntry* entry = (DataWinAudoEntry*) malloc(sizeof(DataWinAudoEntry));
    entry->data = data;
    entry->size = data_size;
    debugf("[n64_datawin] audo[%d]: %lu bytes\n", audo_idx, (unsigned long)data_size);
    return entry;
}

int datawin_get_txtr_count(DataWinIndex* idx) {
    DataWinChunkEntry* e = datawin_find_chunk(idx, "TXTR");
    if (!e) return 0;

    int fd = dfs_open(idx->path);
    if (fd < 0) return 0;

    dfs_seek(fd, (int)(e->file_offset + 8), SEEK_SET);
    uint8_t buf[4];
    int n = 0;
    if (dfs_read(buf, 1, 4, fd) == 4) n = (int) le32(buf);
    dfs_close(fd);
    debugf("[n64_datawin] txtr count: %d\n", n);
    return n;
}
