# .sav Format — v0 Specification (Draft)

> **Status:** Version 0. No version field in the binary — version is implicit. Layout changes silently corrupt or override existing saves.

---

## What Is SAV?

`.sav` is the persistence format for the Shūyatō engine. It stores one save instance — one player's complete session state — as a lightweight binary container. It has no opinion about what that state means. It provides two storage primitives: a fixed flag array for simple integer state, and a named entry table for arbitrary binary blobs. The engine provides hooks for both; the game decides what goes where.

Soft ceiling: **32 KB** total file size (matching N64 Controller Pak budget). No hard enforcement in the format itself.

---

## Binary Layout

**Byte order: little-endian.**

```
[ File Header      ]  32 bytes
[ Flag Table       ]  256 × 4 bytes  =  1024 bytes
[ Entry Index Table]  entry_count × 32 bytes
[ Entry Data Region]  variable
```

---

### File Header — 32 bytes

```
Offset  Size  Type      Field             Notes
──────  ────  ────────  ────────────────  ──────────────────────────────────────
0x00    4     char[4]   magic             "SAV\x1A" (0x53 0x41 0x56 0x1A)
0x04    2     uint16    entry_count       Number of named entries
0x06    2     uint16    flags_offset      Offset to flag table (always 32 in v0)
0x08    4     uint32    index_offset      Offset to entry index table
0x0C    4     uint32    data_offset       Offset to entry data region
0x10    4     uint32    data_size         Total used bytes in data region
0x14    12    uint8[12] reserved          Zeroed
```

**C struct:**
```c
typedef struct {
    char     magic[4];
    uint16_t entry_count;
    uint16_t flags_offset;
    uint32_t index_offset;
    uint32_t data_offset;
    uint32_t data_size;
    uint8_t  reserved[12];
} SAVHeader;   /* little-endian; 32 bytes */
```

---

### Flag Table — 256 × 4 bytes (1024 bytes)

Always 256 slots. Always at offset 32. Each slot is a `uint32` — use it as a boolean, counter, enum, or raw integer. Unused slots are `0x00000000`. The engine makes no distinction between an unset flag and a flag explicitly set to zero.

```c
uint32_t flags[256];
```

**Partial update:** to change flag N without touching anything else, seek to `32 + N * 4` and write 4 bytes. No rewrite needed.

---

### Entry Index Table

Located at `index_offset` (immediately after the flag table in v0: offset 1056). One 32-byte record per named entry.

```
Offset  Size  Type      Field         Notes
──────  ────  ────────  ────────────  ────────────────────────────────────────
0x00    16    char[16]  name          UTF-8, null-padded. Entry identifier.
0x10    4     uint32    data_off      Offset into data region (from data_offset)
0x14    4     uint32    data_size     Byte size of this entry's data
0x18    1     uint8     type_hint     0x00 = raw binary, 0x01 = text (hint only)
0x19    7     uint8[7]  reserved      Zeroed
```

Names are capped at 15 characters + null. Name lookup is linear scan — entry count is expected to be small.

---

### Entry Data Region

Located at `data_offset`. Entry payloads stored contiguously in index order. Contents are opaque — the format does not parse, validate, or type-check them.

**Partial update (same-size or smaller):** seek to `data_offset + entry.data_off`, write new bytes, update `entry.data_size` in the index record. Data region is not compacted — unused trailing bytes from a shrink are left as garbage and ignored.

**Larger replacement or new entry:** requires a full file rewrite. The format has no free-block list or fragmentation management.

---

## Validation Rules

A `.sav` file is **rejected** if:

- `magic` ≠ `"SAV\x1A"`
- `entry_count` × 32 + `index_offset` > file length
- Any entry's `data_off + data_size` exceeds `data_size` in the header

No version check. No checksum. Corruption from a mismatched layout is silent.

---

## File Size Formula

```
32                          (header)
+ 1024                      (flag table, always 256 × 4)
+ entry_count × 32          (entry index table)
+ Σ data_size[i]            (entry data payloads)
```

**Example — minimal town save (flags + 3 entries):**
```
32 + 1024 + (3 × 32) + entry_data  =  1152 + entry_data bytes
```

---

## Example Usage Patterns

**Simple flag toggle (e.g. "has_talked_to_shopkeeper"):**
```
flags[12] = 1   →   seek to offset 32 + (12 × 4) = 80, write uint32 1
```

**Named text entry (e.g. player name):**
```
entry name = "player_name\0"
entry data = "Rainbird\0"   (9 bytes, type_hint = 0x01)
```

**Named binary blob (e.g. town tile map):**
```
entry name = "town_tiles\0"
entry data = [raw binary, arbitrary size]  (type_hint = 0x00)
```

---

## Notes

- No versioning. If the game changes what flag 43 means between builds, old saves silently produce wrong behavior. Version management is the game's responsibility, not the format's.
- `type_hint` is informational only — the engine does not enforce or transform based on it.
- One `.sav` file per save instance. Multiple player slots = multiple `.sav` files.
- The format is intentionally dumb. It is a container with an address book, nothing more.
