# Modern ADT loading (Shadowlands 9.2.7+)

This document describes how Noggit loads terrain (`*.adt`) for modern
projects, what happens when a file is damaged or incomplete, and how the
legacy WotLK loader stays untouched. It is the result of the work tracked
as *"Stabilize Shadowlands 9.2.7 ADT loading"* (issue #1).

## The problem

Before this change, three separate code paths each re-implemented a chunk
walk over modern ADT files, and each carried legacy WotLK assumptions:

1. **The async loader gate** verified that ROOT, `_tex0` and `_obj0` all
   existed, were walkable and contained exactly 256 MCNK chunks - and
   refused the *whole tile* when any of the three had a problem. A single
   damaged texture chunk made the terrain unloadable, and a legacy ADT
   inside a modern project was refused instead of being routed to the
   legacy loader that understands it.
2. **The terrain loader** (`MapTile::finishLoadingShadowlandsTerrainOnly`)
   never read the MHDR chunk. Tile flags therefore stayed 0, MFBO flight
   bounds were never loaded, and saving such a tile wrote `MHDR.flags = 0`
   back to disk. A parse failure in any one of the 256 MCNK chunks threw
   and lost the entire tile. Verbose per-chunk diagnostics were hardcoded
   to tiles 34,49 of every map.
3. **The UID fix** (`MapIndex::fixUIDs`) used legacy MHDR-relative offsets
   (`Header.mddf + 0x14`, ...) on *every* ADT. On a modern split ROOT file
   those offsets are all zero, so it seeked to garbage, indexed
   `modelFilenames[entry.nameID]` out of bounds, and its rewrite phase
   called the legacy `finishLoading()` on modern tiles. It also left
   `_uid_fix_all_in_progress` stuck at `true` when the user cancelled on a
   model loading error.

## The shared reader module

All parsing now lives in one Qt-free module:

    src/noggit/formats/adt/ModernAdtReader.hpp
    src/noggit/formats/adt/ModernAdtReader.cpp

(`namespace Noggit::Formats::ADT`). Because it has no Qt, CASC or OpenGL
dependencies it compiles and runs offline, and its behaviour is covered by
a synthetic-file harness: layout classification, MHDR/MFBO extraction,
truncated/oversized/unknown chunks, MVER checks, TEX0 parsing (MDID/MHID/
MTXP/MCNK/MCLY/MCAL), alpha decoding (RLE, raw, 4-bit, fallbacks, bounds
checks) and OBJ0 parsing (MDDF/MODF, alignment, truncation).

Struct layouts are pinned with `static_assert` (`MHDR` = 0x40,
`MapChunkHeader` = 128, `ENTRY_MDDF` = 36, `ENTRY_MODF` = 64).

### Issue model

Every finding is reported as an `AdtIssue` with a severity:

| Severity  | Meaning                                                        |
|-----------|----------------------------------------------------------------|
| `info`    | Worth a debug line (unknown chunk skipped, fallback used, ...) |
| `warning` | Recovered from something unusual                               |
| `error`   | Something is broken; data was dropped or the file is unusable  |

Callers decide how loud to be; nothing is silently swallowed and nothing
logs per-chunk spam without a cap (`max_subchunk_issue_messages`).

### `scanAdt` / `classifyAdtLayout`

`scanAdt` walks the top-level chunk structure and reports:

* `structurally_valid` - no truncated chunk, no trailing bytes;
* `mver` and whether it is present;
* `has_mtex / has_mmdx / has_mwid / has_mwmo / has_mddf / has_modf / ...`
  marker flags;
* the parsed `MHDR` (when present) and the MFBO flight-bounds extents
  (`mfbo_valid` + `mfbo`, only when the chunk is exactly 36 bytes);
* the absolute offset of every MCNK chunk (`mcnk_offsets`);
* unknown top-level chunks (fourcc -> count) and per-MCNK subchunk
  anomalies (optional, message-capped).

Payload sizes inside an MCNK are **not** 4-byte aligned in retail files
(MCNR is 435 bytes), so the walk advances strictly by declared size and
never assumes alignment.

`classifyAdtLayout` then decides what a file is:

| Layout               | Rule                                                                |
|----------------------|---------------------------------------------------------------------|
| `legacy_single_file` | any of MTEX/MMDX/MMID/MWMO/MWID/MDDF/MODF present (name-list era)    |
| `modern_split_root`  | structurally valid, MVER == 18 and exactly 256 MCNK chunks          |
| `unusable`           | everything else                                                     |

### `parseTex0` / `decodeModernAlpha`

`parseTex0` decodes the `_tex0` part:

* `MDID` (diffuse FileDataIDs), `MHID` (height FileDataIDs),
  `MTXP` (16-byte height-texturing parameters: flags, height scale,
  height offset; flag bit 0 disables the height texture, `uvScale` =
  `(flags >> 4) & 0xF`);
* 256 MCNK containers, each with `MCLY` layer entries (16 bytes:
  `texture_id` -> MDID index, flags, `alpha_offset`, `effect_id`; capped
  at `MAX_TEXTURE_LAYERS` = 16) and one `MCAL` blob;
* each layer's `texture_id` is resolved to its FileDataID, height ids and
  MTXP parameters are applied (MTXP may appear anywhere in the file).

WMO-only tiles (Torghast, ...) legitimately have empty MCLY lists.

`decodeModernAlpha` decodes one layer's alpha into exactly 4096 bytes:

* `FLAG_ALPHA_COMPRESSED` (0x200): RLE - control byte bit 7 = fill run
  (low 7 bits = count, max 127, next byte = value), else copy run;
* otherwise raw when the stored slice is >= 4096 bytes;
* fallback for files written with 4-bit alpha: packed nibbles
  (>= 2048 bytes, low nibble first, `v | (v << 4)` upscale);
* any decode that cannot produce exactly 4096 bytes fails cleanly.

### `parseObj0`

Decodes the `_obj0` part into `m2_placements` (`ENTRY_MDDF`) and
`wmo_placements` (`ENTRY_MODF`). Modern files reference assets by
FileDataID through `nameID` - there are no MMDX/MWMO name lists.
Misaligned or truncated chunks set `ok = false` with an error issue.

## Load routing (the gate)

`AsyncLoader` runs `run_shadowlands_split_adt_gate()` for every root ADT
in a Shadowlands/Retail/Forever project. The gate returns one of:

| Route            | Condition                                          | What happens                                     |
|------------------|----------------------------------------------------|--------------------------------------------------|
| `refuse`         | ROOT missing/unopenable/unusable                   | tile is not loaded; `error_on_loading()`          |
| `legacy_loader`  | ROOT classified `legacy_single_file`               | falls through to the untouched legacy `finishLoading()` |
| `modern_terrain` | ROOT classified `modern_split_root`                | `finishLoadingShadowlandsTerrainOnly()`           |

A usable ROOT is **never** refused because of TEX0 or OBJ0 problems any
more. Those parts are scanned for diagnostics, and their failures degrade
the tile instead of killing it:

* broken/missing `_tex0` -> tile loads **without terrain textures**;
* broken/missing `_obj0` -> tile loads **without model/WMO placements**.

Every problem is logged with the `[ModernADT]` tag, severity-mapped
(errors to `LogError`, warnings to `Log`, info to `LogDebug`).

## Terrain loader

`MapTile::finishLoadingShadowlandsTerrainOnly()` now:

1. Runs the shared `scanAdt` once (the gate already reported detailed
   MCNK validation; the loader does not double-log it). Missing 256 MCNK
   chunks is still fatal - there is no terrain to show.
2. **Reads MHDR into `mFlags`**, so tile flags survive a save/load round
   trip. When flag bit 0 is set it loads the MFBO flight bounds into
   `mMinimumValues` / `mMaximumValues` using the same min/max-inversion
   maths as the legacy loader (a missing/invalid MFBO chunk is reported,
   not fatal).
3. Loads TEX0 through `parseTex0`; failures degrade to "no textures"
   (`[ModernADT][TexDiag]` lines explain what was lost).
4. Builds all 256 chunks. **A chunk that fails to parse no longer takes
   the tile with it**: the failure is caught, a flat placeholder chunk
   (via the `init_empty` MapChunk path, with an empty `TextureSet` so
   downstream consumers never dereference null) is inserted at that grid
   position, and one aggregated `[ModernADT]` error reports the count and
   the first message. The other 255 chunks stay fully editable.
5. Recalculates tile-local normals, initialises render data and finishes
   the tile exactly as before.

The old hardcoded diagnostics for tiles 34,49 are gone. Verbose per-chunk
dumps (height diagnostics for the first 4 chunks, TEX0 layer tables) are
now opt-in for *any* tile via the setting:

    modern_adt_verbose_diagnostics = true

(QSettings, i.e. `noggit.ini` / registry; default `false`.)

## Objects (attachment flow)

`AsyncObject`'s modern attachment loader uses `parseObj0` instead of its
own walk. Unusable OBJ0 data degrades to "no placements" (state machine
still finishes cleanly) instead of throwing; each placement is resolved
through the listfile (`getPath(nameID)`) and constructed with
`FileKey(path, nameID)` so the CASC id stays authoritative.

## UID fix on modern maps

`MapIndex::fixUIDs` was legacy-only and performed undefined behaviour on
modern files. It now:

* opens each root ADT safely (exceptions are caught and reported per
  tile) and classifies it with the shared reader; unusable tiles are
  skipped in **both** phases and their ADTs stay untouched;
* **legacy tiles**: identical MHDR-relative reads as before, plus a new
  guard that skips (and reports) MDDF/MODF entries whose `nameID` is
  outside the MMDX/MWMO list instead of indexing out of bounds;
* **modern tiles**: reads placements from `_obj0` via `parseObj0`,
  resolves each `nameID` FileDataID through the listfile (unresolved ids
  are reported once per tile, entry skipped), and normalises MODF
  `scale == 0` to 1024 (1.0) like the runtime attachment flow;
* the rewrite phase dispatches per tile: modern -> 
  `finishLoadingShadowlandsTerrainOnly()`, legacy -> `finishLoading()`,
  each inside try/catch so one bad tile cannot abort the operation;
* `saveTile` failures are now logged;
* the `cancel_on_model_loading_error` early return resets
  `_uid_fix_all_in_progress` (it used to stay `true` forever, which kept
  the app believing a UID fix was running).

The duplicate filter and tile-extents filter are shared between both
layouts and behave exactly as the legacy code did.

## Failure-mode summary

| Situation                                   | Before                    | Now                                        |
|---------------------------------------------|---------------------------|--------------------------------------------|
| Legacy ADT in a modern project              | refused                   | routed to the legacy loader                |
| Missing/damaged `_tex0`                     | whole tile refused        | tile loads without textures                |
| Missing/damaged `_obj0`                     | whole tile refused        | tile loads without object placements       |
| One damaged MCNK in ROOT                    | whole tile lost (throw)   | flat placeholder chunk + aggregated error  |
| Unknown top-level chunk                     | refusal (gate)            | skipped + info log                         |
| MHDR flags / MFBO on modern tiles           | ignored (flags lost on save) | `mFlags` + flight bounds loaded         |
| UID fix on a modern map                     | OOB reads, legacy loader  | OBJ0 + FileDataID resolution, modern loader|
| UID fix cancelled on model error            | `_uid_fix_all_in_progress` stuck | flag reset                          |

## Legacy behaviour

Nothing changes for WotLK projects: the legacy gate is not consulted for
non-modern projects, `finishLoading()` is untouched, and the legacy
branches inside `fixUIDs` keep their original reads (only the new
out-of-bounds guard was added, which previously was a crash/UB).
