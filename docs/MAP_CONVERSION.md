# Two-way WotLK / modern map conversion

Status: **implemented** - Qt-free core (`src/noggit/formats/conversion/`),
harness-proven round-trip, and the "Tools → Convert map (WotLK <->
modern)..." dialog in the editor window.

This document describes the explicit, user-driven conversion between the two
map file families Noggit supports:

| Family | Client | ADT layout | WDT | WDL | Objects | Liquids | Textures |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **WotLK** | 3.3.5 | one monolithic `.adt` per tile (`MVER/MHDR/MCIN/MTEX/MMDX/MMID/MWMO/MWID/MDDF/MODF/MFBO/MCNK...`) | `MPHD` (8 bytes of flags, rest unused) + `MAIN` | `MWMO/MWID/MODF` placements | referenced by **file name** | per-chunk `MCLQ` | `MCLY.textureID` = byte offset into `MTEX` |
| **Modern** | Shadowlands 9.2.7 (the fork's "modern" family) | split parts per tile: `.adt` (ROOT), `_tex0.adt`, `_obj0.adt`, `_obj1.adt`, `_lod.adt` | `MPHD` with `0x200` (MAID) + fdid columns, `MAIN`, `MAID` | `MLMD/MLMX` (+ `MLDD/MLDX`) | referenced by **FileDataID** | top-level `MH2O` (in ROOT) | `MDID` fdid table; `MCLY.textureID` = index into `MDID` |

Per the README roadmap this is an **explicit** operation: saving a project
never converts it, and opening a map never converts it. Conversion runs
file-to-file, so it also works for maps that cannot be fully loaded in the
editor (missing/partially installed assets), and it needs no GL context.

## Shape of the feature

```
source files (MPQ/CASC via ClientData, or a loose folder)
        │
        ▼
Noggit::Formats::Conversion::convertMap(options, source)   ← Qt-free core
        │
        ├─ ConversionReport.files            → converted bytes, written by the caller
        ├─ ConversionReport.fdid_assignments → manifest: every FileDataID the
        │                                       target client must know (listfile /
        │                                       server-side registration)
        ├─ ConversionReport.suggested_registration_fields
        │                                    → Map.db2 row for the converted map,
        │                                       mergeable into the project's
        │                                       extraData/modernMapRegistrations.json
        └─ warnings / errors / stats         → shown to the user, saved next to output
```

The core library (`src/noggit/formats/conversion/`) contains **no Qt and no
logging**; every decision that needs the application (reading files, resolving
FileDataIDs through the listfile) is injected through
`ConversionFileSource` callbacks. That keeps the whole converter unit-testable
with the standalone g++ harness, like the other format modules.

A thin Qt dialog ("Tools → Convert map…", added later in the implementation
plan) collects: source (current project's ClientData or an explicit folder),
map picker, target format, output directory; runs the conversion on a worker
thread; writes the files + `conversion_manifest.json`; and offers to merge the
Map.db2 registration into the project.

## Direction 1: WotLK → modern (Shadowlands 9.2.7)

Per tile (`<map>_<x>_<y>.adt` monolith → `ROOT` + `_tex0` + `_obj0`):

* **ROOT**: `MVER(18)` + `MHDR` (only `mcin`/`mh2o`/`mfbo` offsets used, the
  name-table fields zeroed) + recomputed `MCIN` + 256 `MCNK`. The 128-byte
  chunk header is copied through (WotLK and 9.2.7 share the v18 header, both
  use `holesHighRes`); fields owned by other parts are adjusted:
  `nLayers` kept, `ofsLayer/ofsAlpha/sizeAlpha/ofsRefs` zeroed (the modern
  client reads layers/alpha from TEX0 and placements from OBJ0),
  `ofsLiquid/sizeLiquid` zeroed (liquids live in the top-level `MH2O`).
  Sub-chunks `MCVT/MCNR/MCSH/MCSE` are **byte-identical between the two
  formats** and are copied verbatim (this is the same fact the native modern
  writer builds on). `MCCV` does not exist in 3.3.5; nothing to carry.
  `MFBO` passes through when present.
* **TEX0**: `MVER` + `MDID` (deduplicated fdid list built from the tile's
  `MTEX` strings via the listfile; unresolvable custom textures get assigned
  ids) + 256 `MCNK{MCLY, MCAL}`. Per-chunk `MCLY` entries keep
  flags/effectID; `textureID` is rewritten from the MTEX byte offset to the
  MDID index. Alpha maps are decoded from the WotLK encoding (uncompressed
  4096, TBC-era packed low-nibble when the WDT says small alpha, or WotLK RLE
  `fill/copy` entries) and re-emitted as modern `MCAL`: RLE when it shrinks
  the payload (`FLAG_ALPHA_COMPRESSED` kept), otherwise raw 4096.
* **OBJ0**: `MVER` + top-level `MDDF` + `MODF` (the verified 9.2.7 layout the
  fork's `parseObj0` consumes). `nameID` becomes the FileDataID of the
  referenced `MMDX`/`MWMO` file; `MODF.flags |= 0x8`
  (name-is-fdid). All other fields copy through. Per-chunk `MCRD/MCRW`
  reference lists are dropped — 9.2.7 OBJ0 does not carry them, and doodads
  attach per tile, exactly like the modern loading path already does.
* **Liquids**: per-chunk `MCLQ` blocks → one top-level `MH2O` chunk (256
  `MH2O_Header` records + information/mask/height payloads) using the same
  layout Noggit's own MH2O writer emits: tight bounding box per layer,
  8-byte presence mask only when the layer does not fill its box,
  heights/uv/depth planes per `LiquidVertexFormat`. Type mapping follows the
  editor's established table: mclq ocean(1) → `LIQUID_OCEAN`, slime(3) →
  `LIQUID_SLIME`, river(4) → `LIQUID_WATER`, magma(6) → `LIQUID_MAGMA`
  (or `LIQUID_Green_Lava` with the project option); vertex formats
  water/ocean → `LVF_HEIGHT_DEPTH` (ocean at height 0 → `LVF_DEPTH`),
  magma/slime → `LVF_HEIGHT_UV`. `fishable`/`fatigue` tile bits move into the
  16-byte `MH2O_Attributes`. Legacy downports that already contain `MH2O`
  pass through unchanged.
* **MTXP/MHID**: WotLK has no height texturing → omitted (both are optional
  in TEX0).

Map level:

* **WDT**: legacy `MVER/MPHD/MAIN` (+ `MWMO/MWID/MODF` for WMO-only worlds)
  → modern `MVER/MPHD/MAIN/MAID`. `MPHD.flags |= 0x200`; the fdid columns
  (lgt/occ/fogs/mpv/tex/wdl/pd4) stay 0 unless the caller resolves them —
  converted custom maps render fine without them (verified by the fork's WDT
  work). `MAIN` flags/async ids pass through. For WMO-only worlds the global
  `MODF` is re-emitted with fdid name ids and `flags |= 0x8`, and
  `MWMO/MWID` are dropped (the modern layout). **MAID**: every tile part the
  converter writes gets an assigned FileDataID (root/tex0/obj0; obj1/lod/
  minimap = 0), recorded in the manifest — the server/listfile side must
  register exactly these ids for the client to find the files.
  Magma/slime blocks map to `LVF_HEIGHT_UV` (the block's uv words pass
  through as the MH2O uv plane), flat zero-height ocean to `LVF_DEPTH`.
* **WDL**: `MVER/MAOF/MARE/MAHO` pass through (identical in both families);
  legacy `MWMO/MWID/MODF` → `MLMD` (40-byte records: MODF without extents,
  nameID = fdid) + `MLMX` (28-byte `min/max/radius` bounds from the MODF
  extents). `MSSN` and friends are not synthesized.
* **Manifest**: JSON next to the output — every fdid assignment (path, role,
  whether it existed in the client listfile or was newly allocated), the
  suggested Map.db2 registration row (mirroring
  `extraData/modernMapRegistrations.json` field names), and the full
  warning/error list. Newly allocated ids come from a configurable base
  (default 900 000 000) so they stay clear of retail ids.

## Direction 2: modern → WotLK

Per tile (ROOT + TEX0 + OBJ0 → monolith):

* `MDID` fdids → `MTEX` path strings via the listfile. **Unresolvable
  FileDataIDs are the one hard loss**: the converter emits a placeholder path
  (`missing\fdid_<id>.blp`) and a warning — the user must rename/convert
  those assets or fix the listfile. `MMDX/MMID/MWMO/MWID` likewise.
* Per-chunk TEX0 `MCLY/MCAL` → per-chunk `MCLY` sub-chunk + alpha decoded
  with the verified `decodeModernAlpha` (RLE / raw / packed fallbacks) and
  re-emitted uncompressed (4096, or 2048 packed low-nibble when the target
  WDT says small alpha) — universally readable by 3.3.5 and by Noggit's
  legacy path. `textureID` rewritten to the MTEX byte offset.
* `MHID/MTXP` (height texturing) have no WotLK equivalent → dropped with a
  per-tile warning (documented loss).
* OBJ0 `MDDF/MODF` (fdid) → global `MDDF/MODF` (name offsets) + name tables;
  `MODF.flags &= ~0x8`. Per-chunk `nDoodadRefs/nMapObjRefs/ofsRefs` are set
  to 0 and no `MCRD/MCRW` are emitted — the same shape Noggit's legacy
  serializer has written for WotLK projects for years (instances attach per
  tile).
* `MH2O` → per-chunk `MCLQ` (inverse of the mapping above; modern vertex
  metadata formats are decoded with the fork's inference rules first).
  `_obj1`/`_lod` parts and minimap textures have no WotLK equivalent and are
  dropped (warning); `_tex1`/`_tex2` layers beyond what MCLY supports are
  clamped with a warning.
* `MCCV`-bearing modern tiles: vertex shading has no 3.3.5 equivalent →
  dropped with a warning.

Map level:

* **WDT**: `MAID` dropped; `MPHD.flags &= ~0x200` (fdid columns zeroed);
  `MAIN` passes through (bit 0 = has ADT in both). Global-WMO maps get
  `MWMO/MWID/MODF` back from the fdid MODF.
* **WDL**: `MLMD/MLMX` → `MWMO/MWID/MODF` (extents from MLMX, `radius`
  dropped); `MLDD/MLDX` (horizon M2s) have no legacy WDL slot → dropped with
  a warning; `MAOF/MARE/MAHO` pass through.
* Output paths use the WotLK conventions (`World\Maps\<name>\<name>_x_y.adt`,
  `MPHD` 8-byte flags); the map is directly usable by a 3.3.5 server/client
  once the missing-asset warnings are addressed.

## Client generations: Shadowlands 9.2.7 vs retail 12.x

`MapFormat::modern` says "not WotLK", but post-WotLK clients are not one
family. A Shadowlands client and a current retail client (12.x) agree on the
ADT layout and disagree on the map-level file set, and their listfiles resolve
different FileDataIDs for the same path. The converter names the families
explicitly so conversion between them is expressible - and so a save aimed at
one client never silently writes the other's file set.

| | `MapGeneration` | ADT | WDT | WDL |
| --- | --- | --- | --- | --- |
| WotLK | `wotlk_3_3_5` | monolithic, legacy layout | `MPHD`+`MAIN` | `MWMO/MWID/MODF` |
| Shadowlands | `shadowlands_9_2_7` | split parts, MVER 18 | + `MPHD` fdid columns, `MAID` | + `MLMD/MLMX`, `MSSN/MSSC/MSSO` |
| Retail | `retail_12_x` | split parts, MVER 18 (same layout) | + `MAI2` (12.0.5+) | + `MSSF` (Dragonflight+) |
| Forever | `forever_1_60_x` | Shadowlands family (beta assumption, documented) | as Shadowlands | as Shadowlands |

`generationFeatures()` exposes that table; `detectModernGeneration(wdt, wdl)`
reads it back off real bytes - MAI2 in the WDT or MSSF in the WDL means
retail, their absence means Shadowlands (the family the editor saves in).

Conversion rules, by file kind:

- **ADT parts** - `convertAdtPartBetweenGenerations`. The two modern families
  share the ADT layout, so between them a part converts by validation and
  verbatim copy: the file must walk clean and carry MVER 18, and is then
  unchanged. WotLK on either end is refused - that is a `convertMap`
  direction, not a generation step.
- **WDT** - `convertWdtBetweenGenerations`. Down (retail -> Shadowlands)
  strips MAI2 byte-exactly and keeps everything else verbatim. Up
  (Shadowlands -> retail) passes the file through and *reports* the missing
  MAI2 instead of inventing one: MAI2 is a per-tile FileDataID table, and
  fabricating ids offline would write numbers the target client cannot
  resolve. The correct source for MAI2 is the target client's own copy of the
  WDT - the same self-heal the save path applies to stale overrides.
- **WDL** - `convertWdlBetweenGenerations`. Identical policy for MSSF;
  MSSN/MSSC/MSSO are carried by both families and survive untouched.

`ConversionOptions::target_generation` (default
`shadowlands_9_2_7`) selects the family `MapFormat::modern` means for a run of
`convertMap`: the builders produce the Shadowlands shape as before, and the
WDT/WDL then step to the chosen generation, each strip and each honest absence
landing in the report as warnings.

`generationFromBuildString` maps a client build string - the one
`loadProject` detects from `.build.info` - to its generation: 3.x -> WotLK,
4.x..9.x -> Shadowlands (the oldest modern family the editor authors), 10.x
and up -> retail, 1.60 and up -> Forever, unparseable/unsupported -> a
caller-chosen fallback. Forever's map files are treated as the Shadowlands
family until research shows a divergence (its beta layouts can change between
updates); chunk-wise the two are indistinguishable, so
`detectModernGeneration` never *returns* Forever - only the project/client
version can say a map is Forever.

Selection is user-facing in two places:

* **Tools → Convert map (WotLK <-> modern)...** has a "Modern family" picker
  (Shadowlands 9.2.7 / Retail 12.x / Forever 1.60.x) that defaults to the
  generation of the project's detected client build and feeds
  `options.target_generation`.
* **Editor → Export map for client...** asks which generation to package for
  and steps the exported WDT/WDL to it - see
  [`MODERN_EXPORT.md`](MODERN_EXPORT.md).

Proven by the "map generations" section of `tools/adt_format_check.cpp`.

## Known fidelity limits (both directions)

* FileDataIDs that the source listfile cannot resolve become manifest
  entries (legacy→modern) or placeholder paths (modern→legacy). This is
  inherent: the information does not exist in the files.
* WotLK has no height texturing, no vertex shading (MCCV), no per-tile LOD
  object part, no minimap texture set; modern has no small (TBC-era) alpha,
  no `MWMO/MWID` name tables inside ADTs. Conversions drop what the target
  cannot represent and say so in the report.
* Liquid conversion covers the four basic types (water/ocean/magma/slime,
  incl. green lava and the WMO variants by basic type). Exotic LiquidType
  rows keep their id modern-side; legacy-side they fall back to the basic
  type mapping above.
* The modern ROOT chunk-header fields owned by other parts
  (`ofsLayer/ofsAlpha/ofsRefs/...`) are zeroed rather than pointed across
  files — matching how the fork's modern reader (and the client's TEX0/OBJ0
  loading) actually finds the data. In-game validation on Epsilon is the
  final check for these; the formats are otherwise built from the same
  verified readers/writers the editor uses.

## Implementation status

1. **Core library** `src/noggit/formats/conversion/` (Qt-free) - done:
   `MapConversion.hpp/.cpp` (API, `convertMap` orchestrator, WDT/WDL
   builders both directions, FileDataID allocation with per-path caching,
   manifest/report) and `AdtConversion.cpp` (legacy ADT parse model, modern
   ROOT/TEX0/OBJ0 assembly, modern tile parse, legacy monolith assembly,
   shared alpha RLE codec, MCLQ→MH2O synthesis). Reuses the verified
   readers (`scanAdt`/`classifyAdtLayout`/`parseTex0`/`decodeModernAlpha`/
   `parseObj0`/`parseWdt`) for parsing **and** as post-conversion
   self-checks: every converted part is re-parsed before it is accepted.
2. **Harness tests** (g++, no CMake) - done: a synthetic two-tile WotLK map
   (WDT + WDL + ADTs with objects, MFBO, MCSH/MCSE, MCLQ water, two texture
   layers with alpha) converts to modern and validates with the repo's own
   readers (MAID wiring, MDID indices, alpha plane equality, OBJ0 fdids and
   flag 0x8, MH2O record contents, WDL MLMD/MLMX and MAOF offsets); the
   outputs convert back to WotLK with the listfile fed from the forward
   manifest and round-trip (legacy classification, MTEX names restored,
   MCVT bytes identical, MH2O preserved, classic WDT/WDL chunk order).
   Plus same-format refusal, placeholder paths for unresolvable
   FileDataIDs, and MH2O byte-passthrough.
3. **Qt UI** - done: `src/noggit/ui/tools/MapConversionTool/`, reachable as
   **Tools → Convert map (WotLK <-> modern)...** in the editor window. The
   dialog runs the conversion on a worker thread, writes the files plus
   `conversion_manifest.json` under the chosen output directory, and offers
   to merge the suggested Map.db2 registration into the project's
   `extraData/modernMapRegistrations.json` (validated through
   `ModernMapRegistrations::validate` first). The conversion source is the
   project's client data (CASC/MPQ); loose-folder sources are supported by
   the same adapter API.
4. **Docs/README** - done: this file is linked from the README; conversion
   moved out of "Outside the Current Scope".

Known follow-ups (not blockers):

-   Newly allocated FileDataIDs still need server-side registration
    (listfile/CASC or the private-server equivalent) - the manifest is the
    input for that; no tooling writes into game installs.
-   Retail 12.x / Forever targets are not conversion targets yet; the
    modern side is the Shadowlands 9.2.7 layout the editor saves in.
-   Per-chunk `MCRD`/`MCRW` reference lists are not carried across in
    either direction (placements attach per tile in both editors and in the
    modern client pipeline).
