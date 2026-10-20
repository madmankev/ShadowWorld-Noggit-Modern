# Modern WMO loading — investigation

Tracks upstream issue
[Jedert-ShadowWorld/ShadowWorld-Noggit-Red-Modern#6](https://github.com/Jedert-ShadowWorld/ShadowWorld-Noggit-Red-Modern/issues/6):
*investigate the changes required for loading modern WMO assets correctly,
beginning with Shadowlands 9.2.7.*

Everything below was verified against this tree (`src/noggit/WMO.cpp`,
`WMO.h`, `wmo_liquid.*`, `rendering/WMORender.*`, `rendering/WMOGroupRender.*`),
against wowdev.wiki (`WMO`, `ADT/v18`), and against a runtime log of a
9.2.7 session (`world/wmo/azeroth/buildings/human_farm/farm.wmo`, root plus
two groups, loaded without errors).

## 1. Format and loading differences versus the legacy path

| Area | Legacy (≤ Cata/MoP style) | Modern (Legion/SL 9.2.7) | Status in this tree |
| --- | --- | --- | --- |
| Archive lookup | filename through the listfile | FileDataIDs everywhere; listfile resolves id → path, and a missing path still loads by id | handled (`Listfile::FileKey(path, fdid)`) |
| Textures | `MOTX` name blob + `MOMT` offsets | `MOTX` omitted; `MOMT.texture_offset_*` fields carry FileDataIDs | handled (`uses_file_data_ids`) |
| Doodad names | `MODN` name blob, `MODD.nameIndex` = byte offset | `MODN` omitted; `MODI` fdid array, `MODD.nameIndex` = **index** into it | handled |
| Group files | `<root>_<NNN>.wmo` naming convention | `GFID` chunk (Legion+) carries explicit group FileDataIDs, one block of `nGroups` ids per LOD | **fixed here**: GFID first, name convention as fallback |
| Skybox | `MOSB` filename string | `MOSI` (BfA+) FileDataID, client accepts either | **fixed here**: MOSI first, MOSB fallback |
| Group chunk layout | fixed chunk order, single `MOTV`/`MOCV` | extra chunks (`MOBS`, `MOPB`, …), `MOTV` may carry trailing padding and a second UV set, second `MOCV` = blend-colour alpha | handled (`modern_chunk_layout`, padding/mismatch diagnostics) |
| Batches | trusted | validated; out-of-range batches dropped with a diagnostic | handled |
| Liquids | `MLIQ` trusted | `MLIQ` bounds-validated, invalid ones skipped with a diagnostic | handled |
| Fog | `MFOG` assumed non-empty | `MFOG` may hold zero entries | **fixed here**: empty-`fogs` no longer reads `fogs[0]` out of bounds |

Detection is by **chunk presence**, never by client build number
(`MOMT` before `MOTX`, `MODI`, `GFID`, `MOSI`, `MOBS`/`MOPB`), so a future
client only needs new chunk handlers, not new version branches.

## 2. Where failures originate

- **Archive lookup** — resolved. Unresolved FileDataIDs (texture, doodad,
  skybox, group) now log once per id and degrade gracefully (load by id,
  skip the skybox, fall back to the group naming convention).
- **Format parsing** — version-aware branches were already present and are
  exercised by the 9.2.7 log. This pass fixed the one crash-class defect
  found: `WMOGroup::load()` dereferenced `wmo->fogs[0]` on WMOs without any
  `MFOG` entry (out-of-bounds read on an empty vector).
- **Model references** — `MODI` index → fdid → listfile → `ModelManager`;
  failures are contained and diagnosed once per id.
- **Rendering** — geometry, normals, UVs (incl. the second set), vertex
  colours and batches are wired; portals (`MOPV`/`MOPT`/`MOPR`) are parsed at
  the root but **not** used for occlusion culling yet (follow-up).
- **Crash containment** — `AsyncLoader::process()` catches
  `FileReadFailedError` and `...` around every `finishLoading()`, so a
  malformed WMO marks the object failed instead of terminating a loader
  thread; anything escaping a Qt slot is caught by `SafeApplication`
  (see the error-flood/crash commit).

## 3. Runtime evidence (9.2.7 session log)

`[ModernWMO] Resolved root "…/farm.wmo": material textures=14, doodad
instances=132, MODI entries=140` and two `[ModernWMOGroup] Loaded …` lines,
repeated four times. The repetition is the reference-counted, per-context
async-object cache releasing and re-acquiring the WMO between reload cycles
(map switches / wizard selection), not a cache miss bug —
`AsyncObjectMultimap::emplace` dedupes by `(context, FileKey)` while any
scoped reference lives.

## 4. Diagnostics added by this pass

All are rate-limited to one line per key (FileDataID or file name):

- `[ModernWMO] GFID … holds N entries for M groups …` (short GFID block)
- `[ModernWMO] Unable to resolve skybox FileDataID …`
- `[ModernWMO] Unable to resolve doodad FileDataID … (MODI index …)`
- `Unresolved WMO texture FileDataID …` (was per material per load)
- `[ModernWMOGroup] GFID … not present in the client; trying the name-based fallback.`
- `[ModernWMOGroup] Cannot derive a group file name from …`
- `Error loading WMO …` (was per group per load)

Pre-existing and kept: MOTV padding/absence, MONR/MOTV count mismatches,
dropped render batches, skipped invalid `MLIQ`, per-root and per-group
summaries.

## 5. Not implemented — proposed follow-up issues

1. **Portal occlusion culling** (`MOPV`/`MOPT`/`MOPR` + `MOGI` flags): all
   groups render today; portals would cut interior overdraw.
2. **MOM3 materials (TWW 11.0+)**: when `MOM3` is present the client ignores
   `MOMT` entirely; today such roots fail with a contained error.
3. **SL dynamic lights and ambient volumes** (`MNLD`, `MAVG`/`MAVD`/`MBVD`,
   `MOLV`): visual parity for raids and garrisons.
4. **LOD groups** (`MGI2`, GFID blocks beyond the base LOD).
5. **Animated UVs and doodad colour multipliers** (`MOUV`, `MDDI`).
6. **WMO shader fidelity**: the second-texture shader set
   `{2,3,5,6,21,23}` predates the wiki's 18179/26522 tables; audit against
   them (e.g. shaders 8/9/11-13/15/17 also take two or three textures).
