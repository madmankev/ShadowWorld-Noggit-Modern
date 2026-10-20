# Modern WDT/WDL parsing (issue #2)

How Noggit reads and saves the two map-metadata files - `.wdt` (World
Definition Table: tile availability, header flags, FileDataID tables, global
WMO) and `.wdl` (low-detail horizon height map + horizon object placements) -
on modern clients (Legion through Shadowlands 9.2.7 and later), what the old
legacy-only code got wrong, and how the new reader/writer architecture works.

Thanks to **Natsirt867** for reporting and scoping the issue.

Related documents: [`MODERN_ADT_WRITER.md`](MODERN_ADT_WRITER.md) (native
modern ADT saving), [`MODERN_MAP_REGISTRATION.md`](MODERN_MAP_REGISTRATION.md)
(Map.db2 registration of Noggit-created maps),
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) (what each log line means).

---

## 1. What was broken

The legacy readers assumed the WotLK 3.3.5 file layout and validated it with
`assert()` only - which compiles out in release builds, so every mismatch
turned into a silent misparse instead of an error:

1. **WDT: fixed-order sequential read.** `MapIndex` read `MVER → MPHD → MAIN`
   and then, for global-WMO maps, blindly read the next two chunks as
   `MWMO → MODF`. Modern WDTs interleave other chunks (`MAID`, `MANM`, ...)
   and - on WMO-only maps - carry **no `MWMO` at all**: `MODF.nameID` is a
   FileDataID (MODF flag `0x8`, Legion+). Noggit therefore read the 64 binary
   bytes of `MODF` as the global WMO's *file name* (garbage), and read
   whatever chunk followed as the placement entry.
2. **WDT: forced `FLAG_SHADING` (0x2).** The loader set bit `0x2` on every
   map and marked the WDT as changed. On modern clients `0x2` means
   *"every ADT of this map has MCCV"* - a completely different statement -
   and the forced `changed` flag made Noggit rewrite the WDT on save.
3. **WDT: legacy save clobbered modern files.** `MapIndex::save()` rebuilt
   the WDT from scratch writing only `MVER/MPHD/MAIN(/MWMO/MODF)`, destroying
   `MAID` (the FileDataID table the modern client needs to find the ADTs at
   all), `MANM`, the MPHD FileDataID fields and the modern global-WMO MODF.
   It also zeroed and recomputed `MPHD.flags`, dropping bits like `0x80`
   (height texturing) and `0x200` (has MAID). This ran after
   `ensureModernWdtInProject()` had carefully copied the original modern WDT
   into the project - the copy was immediately overwritten with a legacy
   file (callers: `World` save flow and the map creation wizard).
4. **WDL: horizon WMOs invisible.** Modern WDLs replaced `MWMO/MWID/MODF`
   with `MLMD` (40-byte WMO placements referencing models by FileDataID) and
   `MLMX` (28-byte bounds). The chunk walker recognized and skipped them, so
   `lWMOInstances`/`mWMOFilenames` stayed empty on modern maps and horizon
   low-res WMO matching (`wmoHasLowRes`) never worked.
5. **WDL: legacy rebuild clobbered modern files.** `save_wdl()` rewrote the
   whole file in the legacy format, dropping every modern chunk (`MLDD/MLDX`
   M2 placements, `MLMD/MLMX`, `MSSN/MSSC/MSSO/MSSF` sky scenes, `MAOE`
   ocean, ...) - a retail client cannot use such a WDL.
6. **WDL: unchecked height data.** `MAOF` size, the `MARE` fourcc/size
   (`1090` bytes) and `MAHO` size (`32` bytes) were only `assert()`ed;
   corrupt files read garbage heights instead of reporting the tile.

## 2. New architecture

Two new Qt-free, logging-free, unit-tested format modules do the parsing; the
consumers keep their existing data structures, so legacy behavior is
untouched:

```
src/noggit/formats/wdt/WdtReader.{hpp,cpp}   WDT chunk walker + validator
                                             + in-place patch writer
src/noggit/formats/wdl/WdlObjects.{hpp,cpp}  MLMD/MLMX -> legacy-shaped
                                             ENTRY_MODF + filename list
```

Both return plain data plus a list of `ParseIssue {severity, message}`
(info/warning/error); `MapIndex` and `map_horizon` log them tagged as
`[WDT Load]`, `[WDT Save]`, `[WDL Load]`, `[WDL Save]`. The chunk walkers
tolerate unknown chunks (aggregated per-file `[Debug]` summaries) so future
client generations do not break loading - they only need new cases when
Noggit starts to *use* the data.

### 2.1 WDT load (`MapIndex` constructor)

1. The whole file image is read into `_rawWdt` and parsed by
   `Formats::WDT::parseWdt(data, modern_client)` where `modern_client` is
   `clientData()->version() != WOTLK` (the same check
   `ensureModernWdtInProject()` uses).
2. The walker validates `MVER` (must be 18), `MPHD` (flags plus, since 8.1,
   seven FileDataID fields - the whole 32-byte chunk is kept), `MAIN`
   (exactly 4096 x 8 bytes: per-tile flags + runtime async id) and `MAID`
   (exactly 4096 x 32 bytes when present).
3. `MAIN` tile flags fill `mTiles[y][x].flags` exactly as before; the
   `existsOnDisk()` ADT probe is unchanged.
4. Global WMO resolution:
   - `MWMO` present -> its first zero-terminated name is used (legacy
     behavior, now without the trailing `'\0'` garbage the old code kept).
   - otherwise `MODF` present with flag `0x8` -> `MODF.nameID` is resolved
     through the CASC listfile (`Listfile::getPath(fdid)`) and normalized.
     An unresolvable id is an `[Error]` and disables the global WMO instead
     of loading a garbage name.
   - `MODF` placement (position converted with `math::to_client`, as before)
     lands in `wmoEntry`; scale is normalized to the 1024 = 1.0 convention
     that `WMOInstance` expects (flag `0x4` semantics, Legion+).
5. `FLAG_SHADING` is force-set **only for legacy clients**; on modern
   clients the loaded bit is left alone and no spurious `changed` flag is
   set.
6. Cross-checks produce diagnostics: flag `0x200` without `MAID`, global-WMO
   flag without resolvable entry, `MAIN`-present tiles whose `MAID` root
   FileDataID is 0 (typical for tiles created inside Noggit - see
   §5), MAID present under a legacy client, etc.

### 2.2 WDT save (`MapIndex::save()`)

- **Modern + a WDT was loaded:** the saved file is the loaded image with
  exactly two regions rewritten (`Formats::WDT::applyWdtEdits`): the MPHD
  flags word (recomputed from `mHasAGlobalWMO`/`mBigAlpha`/
  `_sort_models_by_size_class` on top of the *loaded* flags, so unknown and
  modern bits like `0x80`/`0x200` survive) and the 4096 MAIN entry flags
  (async ids untouched). `MAID`, `MANM`, `MWMO/MODF`, MPHD FileDataID
  fields and every unknown chunk are preserved **byte for byte**.
- **Modern + brand-new map (`create_empty`, no source WDT):** the legacy
  writer still runs - `MVER/MPHD/MAIN` is a valid modern subset - but no
  longer forces bit `0x2`. Allocating MAID FileDataIDs for new tiles is a
  documented follow-up (§5).
- **Legacy client:** unchanged byte-for-byte behavior (including the forced
  `FLAG_SHADING`).
- **Refusal + fallback:** if the loaded image did not parse cleanly, patching
  is refused with `[Error]` messages and the legacy writer runs as a last
  resort (loudly logged, since modern chunks will be lost).

### 2.3 WDL load (`map_horizon` constructor)

- The file image is kept in `_raw_wdl`, and the absolute payload offsets of
  every `MARE`/`MAHO` chunk reached through `MAOF` are recorded in
  `_mare_payload_offsets`/`_maho_payload_offsets` for the save path.
- `MAOF` must be exactly 16384 bytes; every entry is bounds-checked, the
  `MARE` fourcc and its 1090-byte size are verified (a bad tile is skipped
  with an `[Error]` instead of reading garbage), `MAHO` must be 32 bytes.
- `MLMD`/`MLMX` payloads are captured during the walk and converted by
  `Formats::WDL::parseModernWdlObjects`:
  - each 40-byte MLMD record (`nameID` = FileDataID, `uniqueID`, `pos`,
    `rot`, `flags`, `doodadSet`, `nameSet`, `scale`) is resolved through the
    listfile; unresolvable ids are skipped with a warning naming the id;
  - resolved names are de-duplicated into `mWMOFilenames` (normalized like
    the legacy `MWMO` path) and the placements become legacy-shaped
    `ENTRY_MODF` entries in `lWMOInstances` (`nameID` rebased to the
    filename list, extents filled from `MLMX`: upper extents first, like
    MODF; scale normalized to 1024 = 1.0 unless MLMD flag `0x4` says the
    stored scale is meaningful);
  - everything downstream (`World` building `horizon.wmos`,
    `wmoHasLowRes` matching regular WMO instances against their horizon
    low-res twins) now works on modern maps exactly like it always did on
    legacy ones.
- `MLDD/MLDX` (horizon M2 placements) remain skipped-with-counts: legacy
  Noggit never rendered horizon M2s either.

### 2.4 WDL save (`map_horizon::save_wdl`)

- **Modern + a WDL was loaded:** heights are patched **in place** inside
  `_raw_wdl` - for every tile with a recorded slot, the 578+512 bytes of
  `MARE` payload and the 32 bytes of `MAHO` payload are overwritten from the
  in-memory `map_horizon_tile`. All other chunks (`MLDD/MLDX/MLMD/MLMX`,
  sky scenes, ocean, ...) survive byte for byte. `regenerate = true` runs
  the same load-every-tile pre-pass the legacy writer used before patching.
  Tiles that have horizon data but no `MARE` slot in the source file are
  counted and reported once (persisting them would require rebuilding the
  file, which would destroy the modern chunks).
- **Modern + no source WDL:** the legacy writer runs (loudly logged) - a
  legacy-format WDL is still better than none for Noggit's own minimap, but
  modern clients additionally expect the ML*/sky-scene chunks.
- **Legacy client:** unchanged.

## 3. Behavior matrix

| Scenario                        | Load                                   | Save                                      |
| ------------------------------- | -------------------------------------- | ----------------------------------------- |
| Legacy client, legacy WDT/WDL   | unchanged (plus real validation logs)  | unchanged legacy writers                  |
| Modern client, retail WDT       | chunk walker; MAID parsed; no forced 0x2 | in-place patch of MPHD flags + MAIN     |
| Modern WMO-only map (MODF fdid) | name resolved via listfile             | MODF/MAID preserved byte for byte         |
| Modern WDL with MLMD/MLMX       | horizon WMOs loaded + listfile-resolved | MARE/MAHO patched in place                |
| Modern, new map (`create_empty`) | n/a                                    | legacy writer, subset-valid, no forced 0x2 |
| Corrupt file                    | error diagnostics, partial recovery    | patch refused -> legacy fallback + errors |

## 4. Format facts the code relies on

Verified against wowdev.wiki (WDT, WDL/v18, ADT/v18) and the repo's own
`ShadowlandsADTCodec` (`_obj1.adt` rewrite), which already round-trips MLMD/
MLMX records.

**WDT** (flat chunk list, no wrapper): `MVER` (=18), `MPHD` (8 x u32:
flags; since 8.1 the remaining words are `_lgt/_occ/_fogs/_mpv/_tex/_wdl/
_pd4` FileDataIDs), `MAIN` (4096 x {flags u32, asyncId u32}; bit0 = tile
exists, bit1 = all-water, bit2 = runtime "loaded"), `MAID` (8.1+, with MPHD
flag 0x200: 4096 x 8 FileDataIDs - root ADT, obj0, obj1, tex0, lod, map
texture, map texture normals, minimap texture), `MAI2` (12.0.5+), `MWMO`
(zero-terminated name, legacy/WMO-only), `MODF` (64-byte placement; on
modern WMO-only maps `nameID` is a FileDataID and flag `0x8` says so),
`MANM` (9.0.1+ map animations). MPHD flag semantics used here: `0x1` global
WMO, `0x2` legacy "shading" / modern "ADTs have MCCV", `0x4` big alpha,
`0x8` doodads sorted by size class, `0x80` height texturing (MTXP), `0x200`
has MAID.

**WDL**: legacy `MWMO/MWID/MODF` + `MAOF` (4096 absolute u32 offsets, 0 =
no tile) -> `MARE` (17x17 + 16x16 `int16` = 1090 bytes) + optional `MAHO`
(16 x `int16` hole masks). Legion+ replaces the object chunks with
`MLDD/MLDX/MLDL/MLDF` (M2 placements/bounds/lod/fade), `MLMD` (WMO
placements, 40 bytes: ENTRY_MODF without the 24-byte extents block),
`MLMX` (bounds, 28 bytes: min[3], max[3], radius), `MLMB` (per-MLMD u8),
and adds `MSSN/MSSC/MSSO/MSSF` sky scenes, `MSLD/MSLI`, `MAID`, `MWAT`,
`MAOE/MAOC`.

## 5. Known limitations / follow-ups

- **New tiles have no MAID FileDataIDs.** Tiles created inside Noggit are
  flagged in `MAIN` and saved as loose project ADTs, but a modern retail
  client locates ADTs through `MAID`. The load path now *reports* this
  (`N tile(s) ... have no root ADT FileDataID in MAID`); allocating ids and
  writing them belongs to the map-registration/export pipeline
  ([`MODERN_MAP_REGISTRATION.md`](MODERN_MAP_REGISTRATION.md)).
- **Horizon M2s (`MLDD/MLDX`) are still skipped** - parity with legacy,
  which never rendered horizon M2s. The capture pattern for MLMD makes
  adding them straightforward.
- **No native modern WDL writer.** Maps that never had a source WDL get a
  legacy-format file; a full modern writer (ML* chunks, sky scenes) would be
  new-code territory like the ADT writer in issue #3.
- **The UID-fix pass** (`MapIndex`, "fix UIDs" action) still reads ADTs
  with legacy MHDR-relative seeks; on modern split files it should route
  through the codec / `getHighestGUIDFromFile` (which is already
  modern-aware). Tracked separately from this parsing issue.
- **MAI2 (12.0.5+)** is recognized and skipped; parsing it needs a client
  generation Noggit does not target yet.

## 6. Tests

`WdtReader` and `WdlObjects` are Qt-free and covered by a standalone
harness (synthetic legacy WDT, synthetic modern WDT with MAID/MANM/MODF-fdid
/unknown chunks, corrupt + truncated variants, MAID/MAIN consistency, patch
round-trip with byte-level preservation checks, MLMD/MLMX conversion with
fdid resolution, de-duplication, scale normalization, bounds mismatches):

```sh
g++ -std=c++20 -Wall -Wextra -I src -I src/external harness.cpp \
    src/noggit/formats/wdt/WdtReader.cpp \
    src/noggit/formats/wdl/WdlObjects.cpp -o wdtwdl_test && ./wdtwdl_test
# -> ALL PASSED (4181/4181 checks)
```

The Qt-dependent integration points (`map_index.cpp`, `map_horizon.cpp`)
were verified by diff/brace audit against the previous revision, as the
sandbox cannot compile Qt translation units.
