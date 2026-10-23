# Native modern map exporting

Status: **implemented** — Qt-free core
(`src/noggit/formats/export/MapExport.{hpp,cpp}`), harness-proven, wired into
the editor as **Editor → Export map for client...** (`MapIndex::exportMapForClient`).

Loose files in the project folder are what the *editor* reloads. A game client
needs more: every ADT located through the WDT's MAID FileDataID table, the
MPHD-referenced companion WDTs present and registered, a WDL in the modern
format, and — for maps or tiles the client has never seen — listfile entries
and a `Map.db2` row. This pipeline turns the saved project state into exactly
that deployable set, and says precisely what it could not produce.

## What the export does

`Noggit::Formats::Export::exportModernMap(options, input)` (Qt-free, no
logging, no filesystem — the caller supplies file bytes and receives the
deployable set):

1. **Inventory.** The WDT's `MAIN` table is the source of truth for which
   tiles exist. Every present tile must have its ROOT, `_tex0` and `_obj0`
   parts in the set; `_obj1` is warned about when absent and `_lod` only when
   `MPHD` flag `0x100` says the client loads it. A missing required part is a
   hard error naming the tile and the path.
2. **Structural validation.** Every ADT part goes through
   `ModernAdtWriter::validatePart` with the matching expectation
   (Root/Texture/Object/Generic). Nothing reaches the export tree
   unvalidated — the same principle the save path follows per tile, applied
   to the whole map.
3. **MAID assignments.** A modern client loads tiles by FileDataID, so a tile
   created in Noggit with zeroed MAID entries is invisible to it. For every
   present part whose MAID slot is zero, the export uses the id the client's
   listfile already resolves for that path, or allocates a fresh one from
   `fdid_base` (default `900000000`, the converter's convention), skipping
   every id already spoken for in the WDT or known to the client. Slots 5–7
   (baked map texture, normals, minimap) stay zero for new tiles and are
   reported once per map: those images are client-baked and cannot be
   authored offline.
4. **MPHD columns.** The WDL (word 6) and the six aux WDTs (`_lgt`/`_occ`/
   `_fogs`/`_mpv`/`_tex`/`_pd4`, words 1–5, 7) get the same treatment:
   file in the set with a zero column → known id or fresh assignment,
   patched into the exported WDT; column set but file absent → warning
   naming both.
5. **WDT/WDL format sanity.** `MVER != 18`, a too-small MPHD/MAIN/MAID, a
   missing MAID despite flag `0x200`, a legacy-format WDL (MWMO/MWID/MODF
   chunks) — each is reported with what the client expects instead.
6. **The WDT's own id.** `known_fdid` or a fresh allocation becomes the
   registration's `WdtFileDataID` — the "CASC-visible fd for the map" the
   registration format was designed to receive.

The patched WDT exists **only in the export tree**. The project's own WDT is
never modified, so the save path's self-heal (which restores a project WDT
differing from the client original) and the export never fight each other:
deploy the exported WDT, keep editing against the client's.

## Artifacts

Written next to the exported files in `<project>/export/<map directory>/`:

| File | Contents |
| --- | --- |
| `export-manifest.json` | `conversion_manifest.json` conventions: map identity, tile stats, every file with size + FNV-1a 64 content hash, every FileDataID assignment (`fdid`, `path`, `role`, `existed_in_client`), the registration block, all warnings and errors |
| `listfile-additions.csv` | `fdid;path` lines (listfile convention: lowercase, forward slashes) for exactly the ids this export allocated — what a CASC rebuild or server pack needs to make the new files resolvable |
| `map-registration.json` | `extraData/modernMapRegistrations.json` storage format, single entry: `id`, `target_build`, `fields` (`Directory`, `MapName_lang`, `WdtFileDataID`) — mergeable into the project's registrations |

When the project already has a registration row for this map id, the export
also writes the settled `WdtFileDataID` into it directly
(`ModernMapRegistrations::load/upsert/save`), so the editor-side DB2 bridge
and the export agree.

## Editor wiring

`MapIndex::exportMapForClient()` (menu: **Editor → Export map for
client...**):

* Collects candidates — WDT, WDL, six aux WDTs, five parts per `hasTile`
  tile — reading **project-first**: the saved loose file wins, the client's
  own copy fills in everything the project does not override. So the export
  contains the edited terrain where it exists and retail bytes elsewhere,
  which is exactly the set the merge-path saving produces.
* `path_to_fdid` is the client listfile (`normalizeFilenameInternal` →
  `getFileDataID`), so "existed in client" is the client's answer, not a
  guess.
* Writes the tree + artifacts under `<project>/export/<basename>/`, logs a
  `[Map Export]` summary (files written, tiles complete, new ids), the first
  20 warnings/errors inline and the rest by reference to the manifest, and a
  hard **NOT deployable** line when anything failed.

## Deliberate limits

* **No fabrication.** Ids are only invented where an id is all that is
  missing — the file bytes must exist in the set. MAI2 (12.0.5+), sky scenes
  and baked textures are never synthesized; what cannot be produced offline
  is reported (see the generation policy in
  [MAP_CONVERSION.md](MAP_CONVERSION.md)).
* **No Map.db2 binary writing.** Noggit edits against a read-only CASC
  client; the registration artifact carries the column values, and applying
  them to a real `Map.db2` stays with the mod packer/server tooling that
  consumes `listfile-additions.csv` alongside it.
* **WotLK projects** export as-is without MAID/MPHD work; the pipeline is
  modern-format specific (its checks name what a modern client expects).

## Testing

`tools/adt_format_check.cpp`, section "map export pipeline": a fixture map
whose parts come from the real converter — clean export; MAID filled for the
new tile (distinct ids, obj1/lod slots stay zero); MPHD lgt column assigned,
an existing column left alone; exact `listfile-additions.csv` content;
registration fields with `WdtFileDataID`; manifest conventions; referenced-
but-absent aux WDT, missing WDL and zero baked-texture warnings; a MAIN tile
without parts → hard error naming tile and path; a corrupt ROOT → validation
error; no WDT → refused. Build and run:

    mkdir -p /tmp/synth && g++ -std=c++20 -I src -I src/external \
      -o /tmp/synth/harness tools/adt_format_check.cpp \
      src/noggit/formats/adt/ModernAdtWriter.cpp \
      src/noggit/formats/adt/ModernAdtReader.cpp \
      src/noggit/formats/wdt/WdtReader.cpp \
      src/noggit/formats/wdl/ModernWdlWriter.cpp \
      src/noggit/formats/export/MapExport.cpp \
      src/noggit/formats/conversion/AdtConversion.cpp \
      src/noggit/formats/conversion/MapConversion.cpp
    /tmp/synth/harness        # exit 0 = pass

In-app verification: open a modern project, **Editor → Export map for
client...**, then check `<project>/export/<basename>/` for the tree and the
three artifacts, and the log for the `[Map Export]` summary. For a map with
tiles created in Noggit, the summary's `new FileDataID(s)` count and
`listfile-additions.csv` are what the deployment (CASC rebuild / server
pack) has to consume for those tiles to load.
