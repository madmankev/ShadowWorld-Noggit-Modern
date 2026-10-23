# Modern ADT specification vs. this fork

An item-by-item audit of the "native modern ADT" refactor brief against what
this fork actually does, and against the only specification that matters: the
files a modern client ships and loads. Where the brief and retail disagree,
retail wins — the client is the spec, and "compatible" means byte-compatible
with it, not with a description of it.

Ground truth used throughout:

* FormatProbe dumps of retail 9.2.7.45745 files, emitted by this fork's loader
  (`AsyncLoader.cpp`, `[ModernADT][FormatProbe]`), e.g. for azeroth:

      ROOT: [MVER(4) MHDR(64) MCIN(4096) MFBO(36) MH2O(3072) MCNK x256]
            MCNK[0] subs = [MCVT(580) MCNR(435|448) MCSE(0)]
      TEX0: [MVER(4) MAMP(4) MDID(32) MHID(32) MCNK x256]
            MCNK[0] subs = [MCLY(64) MCSH(512) MCAL(6139)]
      OBJ0: [MVER(4) MDDF(54396) MODF(640) MCNK x256]
            MCNK[0] subs = [MCRD(56) MCRW(4)]

* wowdev.wiki ADT v18 / WDT / WDL.
* `tools/adt_format_check.cpp`, a Qt-free harness that links the real writer,
  reader and converter and asserts the conventions below (115 checks).

## 1. Split-ADT architecture — implemented

Every saved tile produces the root ADT plus `_tex0`, `_obj0`, and `_obj1`/`_lod`
when the source carries them.

* Paths and part set: `ShadowlandsADTCodec::makeSource` / `split_path`.
* Merge path (the normal case): `assemble_parts` rewrites only the terrain
  payloads inside the retail parts, so everything this fork does not edit
  survives byte-for-byte.
* Synthesis path (last resort when no usable original exists):
  `synthesize_parts` → `Conversion::convertAdtLegacyToModern`
  (`buildModernRoot` / `buildModernTex0` / `buildModernObj0`).
* Atomic loose-file writes: `save_part` (QSaveFile replace), CASC untouched.

## 2. FileDataID integration — implemented, with two deliberate gaps

* `MDID` in TEX0: written from resolved texture paths
  (`resolve_file_ids` / `lookup_fdid` with `ModernAdtFdidHints`), or preserved
  from retail on the merge path.
* `MDDF` (36 bytes) / `MODF` (64 bytes): `ENTRY_MDDF` / `ENTRY_MODF`, sizes
  asserted; placements carry FileDataIDs, legacy name tables are not emitted.
* Unresolvable assets: placements are dropped together with their per-chunk
  references and the survivors renumbered (`index_remap`,
  `rewrite_obj0_refs`); textures never write FileDataID 0 into `MDID` — a zero
  there is a client-side null dereference, which is exactly the crash class
  this project has been eliminating.
* Injected assets get allocated IDs from a custom base (900000000) via
  `FdidAllocator`, reported in the save summary.
* Gap, deliberate: `MHID`/`MAMP` are preserved by the merge path but not
  synthesized. Legacy sources carry no height textures, and synthesizing an
  `MHID` of zero FileDataIDs would reintroduce the null-dereference class;
  omission is the safe choice until height texturing is editable.
* Spec error: there is no `MPID` chunk in a 9.2.7 `_tex0.adt` (probe above:
  MVER, MAMP, MDID, MHID, MCNK). Writing one would be writing fiction.

## 3. Chunk layout — implemented, with four spec corrections

* `MVER` = 18: `AdtFormatProfile::adt_profile_v18`, enforced by
  `validatePartOrThrow`.
* `MHDR`: flags cleaned on synthesis (`mhdr_flags & 0x1`), offsets recomputed
  against the real layout (`refreshRootBookkeeping`).
* `MH2O` only, never `MCLQ`, in modern output: merge preserves retail MH2O;
  synthesis converts legacy MCLQ to MH2O; a legacy MCLQ chunk header that
  declares size 0 (Noggit's historical bug, fixed in `d5f9b56`) is repaired on
  read from the MCNK header's `sizeLiquid`/`ofsLiquid`.
* `MCNK` subchunks: MCVT, MCNR, MCCV, MCSE in ROOT; the 13 reserved bytes after
  MCNR are decided by accounting, not by inspecting bytes (try-both walk,
  `92d6ec9`); 64-bit high-resolution hole masks handled in
  `merge_chunk_header`.
* TEX0: MCLY + MCAL (RLE or opaque plane, flags reconciled) + MCSH, which lives
  in TEX0 in the split format — the ROOT's `has_mcsh` bit is set exactly when
  TEX0 carries the chunk (`bd6c823`).
* OBJ0: MDDF, MODF, per-chunk MCRD/MCRW — no nested legacy reference lists.
* Spec error: `MCIN` is **not** deprecated. Retail 9.2.7 ROOT files contain
  MCIN(4096); stripping it would diverge from retail. This fork keeps it and
  uses the retail size convention (payload + 8 byte tag), which the harness
  pins.
* Spec error: `MCLV` does not exist in ADT v18 (the WDT's MCLV flag is
  forbidden since 8.x). Nothing to implement.
* Spec error: `MMDX`/`MWMO`/`MTEX`/`MMID`/`MWID` do not belong in a modern
  `_obj0.adt` (probe above). They are legacy monolithic or OBJ1/LOD tables.
* Spec error: `MFBO` is not mandatory; it is gated by MHDR flag bit 0 and
  absent from some retail maps. Merge preserves it; synthesis omits it with
  the flag clear, which is what retail does for maps without it.

## 4. Binary alignment and offsets — implemented, one spec correction

* Chunk sizes exclude the 8 byte tag everywhere (`chunk_tag_size`), asserted
  by the harness on every fixture variant.
* MCNK-internal offsets are relative to the MCNK chunk start, recomputed from
  the emitted layout (`assemble_root_mcnk`, `buildModernRoot`), asserted by the
  harness.
* Spec error: "chunk.size % 4 == 0" is false for retail — the probe shows
  MCAL(6139) and MCRD(56)/MCRW(4) mixes with odd sizes. Padding chunk payloads
  to 4 bytes would break retail parity and desynchronize every reader that
  trusts the size field, which is all of them.

## 5. Crash prevention — implemented and regression-tested

* Liquid and shadow flag bits are reconciled with the chunks actually present
  (`3a58b54`, `bd6c823`): a bit claiming an absent MH2O or MCSH sends the
  client to a table that is not there, which it dereferences.
* MCLQ chunk size is written truthfully and size-0 MCLQ is repaired on read
  (`d5f9b56`).
* MODF extents are sanitized on every write path
  (`Conversion::sanitizeModfEntry`): non-finite positions/rotations become
  zero, non-finite extents become a one-unit box around the placement, and
  inverted corners are swapped per axis. Harness-covered.
* Stale project overrides from another client build can no longer mask retail:
  unusable parts are replaced by the CASC original with a logged reason
  (`a42535e`), and a project WDT differing from the client's original is
  restored, because its MAID FileDataIDs are what the client loads every tile
  by (`769b8e9`).
* The save reports, per tile, which path ran, which parts reached disk, and
  every loss (unresolved textures, dropped placements, invented IDs), so a
  client-side crash can be attributed from the log alone.

## 6. Export pipeline — implemented

Loose files in the project override folder, CASC-ready directory layout
(`world/maps/<map>/…`), atomic per-part replacement, plus the map-level
companions a client expects next to the WDT: the WDT itself, the WDL, and the
MPHD-referenced `_lgt`/`_occ`/`_fogs`/`_mpv`/`_tex`/`_pd4` WDTs
(`map_index.cpp: ensureModernWdtInProject`, `ensureAuxiliaryWdtsInProject`).

On top of that, **Editor → Export map for client...** packages the whole map
for deployment: inventory against `MAIN`, structural validation of every
part, MAID/MPHD FileDataID assignments for tiles and files the client does
not know, and the `listfile-additions.csv` / `map-registration.json` /
`export-manifest.json` artifacts — see
[`MODERN_EXPORT.md`](MODERN_EXPORT.md). The WDL save path writes the modern
format natively (`WDL::writeModernWdl` / `rewriteModernWdl`) — see
[`MODERN_WDT_WDL.md`](MODERN_WDT_WDL.md).

## Verifying any of the above

    mkdir -p /tmp/synth && g++ -std=c++20 -I src -I src/external \
      -o /tmp/synth/harness tools/adt_format_check.cpp \
      src/noggit/formats/adt/ModernAdtWriter.cpp \
      src/noggit/formats/adt/ModernAdtReader.cpp \
      src/noggit/formats/wdt/WdtReader.cpp \
      src/noggit/formats/wdl/ModernWdlWriter.cpp \
      src/noggit/formats/export/MapExport.cpp \
      src/noggit/formats/conversion/AdtConversion.cpp \
      src/noggit/formats/conversion/MapConversion.cpp
    /tmp/synth/harness        # exit 0 = every convention above holds

In the editor, `[ModernADT][FormatProbe]` logs the same inventory for every
retail tile it loads, which is how the retail column of this document is kept
honest.
