# Native modern ADT writer

Status: implemented for the Shadowlands 9.2.7 split-ADT format (`MVER 18`).
The legacy WotLK writer is untouched; WotLK projects keep using it unchanged.

Upstream issue: *Implement native modern ADT writer* — add a native writer
path for modern ADT formats so edited maps can eventually be saved without
converting them back to WotLK first.

## How saving worked before

`MapTile::save` serialized the whole tile in Noggit's canonical **WotLK
single-file ADT** layout, and `ShadowlandsADTCodec::saveFromLegacy` then
grafted chunks out of that legacy buffer into copies of the original modern
split files (`ROOT`, `TEX0`, `OBJ0`, `OBJ1`, `LOD`). That conversion worked,
but the ROOT's bookkeeping was never regenerated:

* `MCIN` (256 × offset/size entries describing where each `MCNK` lives) was
  copied from the original file even though replacing subchunks
  (`MCSE`, `MCSH`, `MCCV`, …) and top-level chunks (`MH2O`, `MFBO`) moves
  and resizes the `MCNK`s.
* `MHDR.mcin/mh2o/mfbo` offsets were copied as well and went stale for the
  same reason.
* `MCNK` header subchunk offsets (`ofsHeight`, `ofsNormal`, `ofsMCCV`,
  `ofsShadow`, `ofsSndEmitters`, …) kept pointing into the original layout.

Noggit's own reader walks chunks sequentially and never noticed, but a retail
client consumes exactly these fields — edited tiles risked being read as
corrupt by the game. There was also no pre-write validation beyond the TEX0
alpha layout.

## The native writer

New module: `src/noggit/formats/adt/ModernAdtWriter.{hpp,cpp}`
(Qt-free, dependency-free, unit-tested outside the app).

### Version awareness

Every format decision lives in an `AdtFormatProfile` (`mver`, MCNK count,
header size, heightmap vertex count, high-res-hole support, …). The current
target is `adt_profile_v18()` — Shadowlands 9.2.7. A later client generation
gets its own profile (and overrides where the format actually diverges)
instead of edits scattered through the writer.

### Native ROOT assembly — `writeRoot`

Walks the **original** ROOT part and regenerates it in original chunk order:

| Chunk | Source |
| --- | --- |
| `MVER` | native (`profile.mver`) |
| `MHDR` | original payload with editor flags patched; `mcin`/`mh2o`/`mfbo` offsets recomputed |
| `MCIN` | **recomputed**: offsets/sizes from the emitted layout; per-entry `flags`/`asyncID` preserved from the original |
| `MCNK` × 256 | native merge (below) |
| `MH2O` | editor payload when the tile wrote one, otherwise preserved |
| `MFBO` | editor payload when `mFlags & 1`, otherwise preserved |
| anything else | preserved byte-for-byte (`MTXP` and any future/unknown chunk) |

`MCNK` merge rules (per chunk):

* The 128-byte header starts from the **original** header and takes
  editor-owned fields from the serialized chunk state: `flags`, `ix`, `iy`,
  `areaid`, `ypos`, `nDoodadRefs`, `nMapObjRefs`, `doodadMapping`,
  `doodadStencil`, and holes. High-resolution holes (`flags.high_res_holes`)
  keep the 64-bit mask stored in `ofsHeight`/`ofsNormal`, with the edited
  low-res 4×4 grid translated into the mask exactly like the proven
  conversion path did.
* Owned subchunks (`MCVT`, `MCNR`, `MCCV`, `MCSH`, `MCSE`) are replaced with
  the editor's payloads **in original order**; owned subchunks the original
  did not carry are appended in canonical order.
* Every other subchunk is preserved byte-for-byte in place.
* Header offsets are recomputed against the emitted layout
  (`ofsHeight`, `ofsNormal` — skipped while the hole mask occupies them —
  `ofsMCCV`, `ofsShadow`/`sizeShadow`, `ofsSndEmitters`/`nSndEmitters`).
  Fields owned by other parts (`nLayers`, `ofsLayer`, `ofsAlpha`,
  `sizeAlpha`, `ofsRefs`, `ofsLiquid`, `sizeLiquid`, positions) are left as
  the original file had them.

The `MHDR` offset **basis** is self-calibrated from the original file
(`MCIN` position vs. stored `mcin` value), so whatever convention the retail
file uses is reproduced exactly; Noggit's legacy `+0x14` convention is the
fallback for hand-built files.

### Staged payload sourcing

`rootInputFromLegacy()` fills the writer's `NativeRootInput` from Noggit's
canonical legacy serialization. This is deliberate and documented in the
header: the *chunk payload* formats (`MCVT`/`MCNR`/`MCCV`/`MCSH`/`MCSE`) are
byte-identical between WotLK and 9.2.7 — only the container differs — and the
container is now assembled natively. `NativeRootInput` is source-agnostic: a
follow-up increment can fill it directly from `MapChunk`/`texture_set` state
and delete the legacy pass without touching the writer. `TEX0`, `OBJ0` and
`OBJ1` still convert (their conversion already produces modern-native
chunks: `MDID` file-data-ids, `MDDF`/`MODF` with FDIDs, `MCRD`/`MCRT`
references); `LOD` is preserved byte-for-byte.

### Validation before writing

`validatePart(bytes, expectation)` structurally checks a part and returns the
problem list; `validatePartOrThrow` raises `ModernAdtValidationError`:

* **Root**: strict chunk walk; `MVER == 18`; `MHDR` is 0x40 bytes; `MCIN` is
  256 entries **and every entry matches the actual MCNK offset/size**;
  `MHDR.mcin/mh2o/mfbo` point at the chunks they name (basis-calibrated);
  exactly 256 `MCNK`s; each has a full header, walks cleanly, has a
  145-float `MCVT`, and its `ofs*` fields point at the right subchunks
  (`sizeShadow`/`nSndEmitters` consistent).
* **Texture**: strict walk; 256 `MCNK`s; `MDID`/`MCLY`/`MTXP` size
  alignment; plus the pre-existing deep alpha-layout rules
  (`validate_tex0_alpha_layout`).
* **Object**: strict walk; `MDDF`/`MODF` entry alignment; 256 `MCNK`s;
  `MCRD`/`MCRW` indices within the placement counts.
* **Generic** (`OBJ1`, `LOD`): strict chunk walk.

Both save paths (`saveNativeRoot` **and** the `saveFromLegacy` fallback)
assemble every part in memory, validate everything, and only then write —
nothing reaches disk unvalidated, and a validation failure never falls back
to a path that would write the same suspect data.

### Save-path flow (`MapTile::save`)

```
modern project:
  serialize canonical tile (unchanged)
  → ModernAdtWriter::rootInputFromLegacy   (payload extraction)
  → ModernAdtWriter::writeRoot             (native ROOT, self-validated)
  → ShadowlandsADTCodec::saveNativeRoot    (convert TEX0/OBJ0/OBJ1,
                                            refresh nothing on ROOT,
                                            validate ALL parts, then write)
  on ModernAdtValidationError → abort save, log, write nothing
  on any other writer error   → log + fall back to saveFromLegacy
                                (which now refreshes ROOT bookkeeping —
                                 MCIN/MHDR repair — and validates too)
legacy project: unchanged single-file write.
```

The fallback's new `refreshRootBookkeeping()` repairs the stale
`MCIN`/`MHDR` offsets of the conversion path itself, so even when the native
writer is bypassed the old corruption class is fixed.

## Testing

Unit-tested outside the app (no Qt, no CASC) with synthetic 256-chunk
original/legacy tiles — see `/tmp/adtwriter` harness pattern:

* extraction: 256 chunks, optional `MCCV`/`MCSH` presence, zero-entry `MCSE`
  distinguished from absent, `MH2O` payload, `MHDR` flags.
* native write: chunk order and byte-preservation of unknown top-level
  chunks and subchunks; `MVER`/`MHDR` regeneration; `MCIN` offsets/sizes
  match the emitted layout with `flags`/`asyncID` preserved; header field
  merges; low-res hole merge (high 16 bits preserved) and **exact** 64-bit
  high-res hole mask semantics; owned-subchunk replace/preserve/append;
  every recomputed `ofs*` verified against actual positions; `MH2O`
  replaced with a different size while `MFBO` is preserved.
* `refreshRootBookkeeping` on a corrupted `MCIN`/`MHDR` reproduces the
  canonical file byte-for-byte.
* validation negatives: bad `MVER`, truncation, `MCIN` mismatch, zeroed
  `MHDR.mh2o` with the chunk present, missing `MCIN` input to `writeRoot`,
  `MCLY` misalignment, 255-MCNK parts, out-of-range `MCRD` indices,
  misaligned `MODF`.

In-app verification (9.2.7 project): save a tile after a height/texture edit
and check for
`[ModernADT][Save] Saved split tile ROOT/TEX0/OBJ0/OBJ1/LOD ...` with no
`Native ROOT writer failed` fallback message; the written ROOT should have
`MCIN` offsets matching the actual `MCNK` positions (any hex viewer).

## Follow-ups (kept extensible on purpose)

* Fill `NativeRootInput` directly from `MapChunk` state and drop the legacy
  serialization pass for modern saves (the writer API already allows it).
* Native `TEX0` (emit `MDID`/`MCLY`/`MCAL`/`MTXP` from `texture_set`) and
  native `OBJ0`/`OBJ1` (placements and LOD directly from instances).
* Per-chunk liquid ownership (`ofsLiquid`/`sizeLiquid` are still preserved
  from the original while `MH2O` is replaced wholesale).
* Profiles for client generations after 9.2.7 as formats diverge
  (e.g. WDC-era changes to object parts).
