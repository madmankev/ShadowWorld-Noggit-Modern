# Troubleshooting modern projects

This page covers the messages Noggit writes to `log.txt` (next to the
executable) when a Shadowlands, Retail, or Forever project does not behave, and
what to do about each of them.

Every entry has the shape:

``` text
<milliseconds since start> - (<file>:<line>): [Error] <message>
```

`[Error]` means something did not work, `[Debug]` is diagnostic detail, and an
entry without a tag is normal progress output.

## "Database definition build/layout not found for X"

The client databases (`Map.db2`, `AreaTable.db2`, `MapDifficulty.db2`, ...) are
described by the DBD definition files in the `definitions` folder next to the
executable. Those files come from the **ShadowWorld-DBD-Definitions** submodule
and list the client builds and layouts they were verified against. Blizzard
changes table layouts between builds, so a client newer than the definitions can
have a table that no definition block describes.

Noggit now handles that instead of giving up:

1. It loads the table with the definition for the detected client build.
2. If that fails, it reads the header of the `.db2` file itself (layout hash and
   column count) and looks for the definition block that provably describes the
   file: a block that declares the same layout hash first, then a block with the
   same number of columns. Ties are broken by the closest build.
3. The substituted table is cached, so the work happens once per session, and one
   line is written to the log:

``` text
[DB2Definitions] MapDifficulty.db2: client build 9.2.7.45745 is not described by
definitions/MapDifficulty.dbd, using the 9.2.0.41462 definition instead
(11 matching columns, 250 records)
```

A definition is only used when it matches the file. If none matches, one error is
logged per table, listing what was tried and why it was rejected:

``` text
[ModernDB2][Map] SoundKit.db2: client build 9.2.7.45745 is newer than the
definitions in definitions. The file has layout 8A1C0F42, 18 columns and 180186
records, but none of the definition blocks describes it:
    9.2.0.41462: 16 columns (does not match the file)
  Update the DBD definitions to read this table.
```

To fix that permanently, update the definitions submodule
(`git submodule update --remote dist/definitions`) or add a block for the missing
build to the `.dbd` file. A block only needs the layouts/builds it covers and one
line per column:

``` text
COLUMNS
int ID
...

BUILD 9.2.7.45745
$id$ID<32>
...
```

`dist/noggit-definitions/db2-shadowlands/Light.dbd` is a worked example of a
single-build definition.

Tables that fail this way keep their legacy bridge data empty, so the features
that read them (sound, weather, zone light, terrain type) stay at their defaults
until the definition exists. Everything else keeps working.

## "Skipped N Map.db2 records while building the map list"

`Map.db2` lists every map that ever shipped, including maps whose data is not
installed in the client (Battle.net installs only download the maps you asked
for, plus garrisons, transports, phased variants, and unannounced content).

Noggit used to write one error per map. It now writes one summary with the
reasons grouped, and the individual maps at `[Debug]` level:

``` text
Skipped 308 Map.db2 records while building the map list, their map data is not
available in the client:
  308 x File read failed. requested={path=world/maps/islandsn/islandsn.wdt}
```

This is not a problem with the project. Those maps cannot be opened because the
client does not contain them. To get them, install the missing content in the
Battle.net client (or point the project at a complete client copy).

## A map I created is gone after a restart (modern projects)

On modern projects the map table is rebuilt from the client's `Map.db2` on
every load, so the legacy `DBFilesClient/Map.dbc` write is not enough. New maps
are therefore also persisted as a project registration in
`extraData/modernMapRegistrations.json`, which the DB2 bridge merges back in at
startup. When it works you will see:

``` text
[MapWizard] Registered map id=2100 in extraData/modernMapRegistrations.json (target build 9.2.7.45745)
[ModernDB2][Map] Map registrations merged=1 skipped=0
```

If the map is still missing after a restart, look for one of:

* `[MapWizard] ... was not registered for the modern pipeline: <reason>` —
  validation rejected the entry (e.g. empty `Directory`, characters outside
  `A-Z a-z 0-9 _`, `InstanceType` above 4). Fix the value and save again.
* `[ModernDB2][Map] Skipping invalid registered map id=...` — the stored
  registration is malformed (usually a hand-edited JSON file); the reason
  follows the message.
* `[ModernDB2][Map] Registered map id=... is already provided by the client`
  (Debug level) — the id collides with a real client map; pick another id.
* `[MapRegistration] Could not parse ...` — the JSON file is corrupt; restore
  it or delete it and re-save the map from the wizard.

Full design, the 9.2.7 field list, and the validation rules are documented in
[`MODERN_MAP_REGISTRATION.md`](MODERN_MAP_REGISTRATION.md).

## "wdl ... skipped N unsupported chunks"

Modern `.wdl` files (the low-detail height map Noggit uses for the minimap)
contain chunks that Noggit does not need for the height data: M2 placements
(`MLDD`, `MLDX`), sky scenes (`MSSN`, `MSSC`, `MSSO`, `MSSF`), ocean and
file-data chunks (`MAOE`, `MAID`, `MWAT`). They are skipped and reported once
per file at `[Debug]` level:

``` text
wdl "World\Maps\TwilightHighlands\TwilightHighlands.wdl": skipped 369 unsupported
chunks (MLDD x123, MLDX x123, MLMB x123)
```

Since the modern WDT/WDL parsing rework, the horizon WMO placements (`MLMD`)
and their bounds (`MLMX`) are no longer skipped: Noggit resolves their
FileDataIDs through the CASC listfile and feeds them into the horizon WMO
lists - look for `[WDL Load] ... loaded N horizon WMO placement(s) from MLMD`.
See [MODERN_WDT_WDL.md](MODERN_WDT_WDL.md).

The height data (`MARE`/`MAHO`, reached through `MAOF`) is still read, so the
minimap is unaffected. A chunk Noggit does not recognise at all is reported
individually, with its four-character name when it has one.

## "Map N did not create a World for the minimap"

The map could not be opened, and the map creation wizard already explained why in
a dialog (usually a missing or unreadable `.wdt`). This line is `[Debug]` now;
look for the wizard's message above it.

If the wizard has no map open, its buttons and the minimap grid refuse to act and
say so once instead of dereferencing a null world:

``` text
Map creation wizard: saving the map needs an open map, but the current map could
not be loaded. Select another map first.
```

## Noggit closed itself / "Exception 0xE06D7363"

`0xE06D7363` is how Windows reports a C++ exception that nobody caught. Noggit
now catches exceptions thrown from user interface events, writes them to the log,
and shows a dialog instead of exiting:

``` text
[Error] Uncaught exception while handling a MouseButtonRelease event for
QPushButton: Could not write G:/project/DBFilesClient/Map.dbc
```

If Noggit still exits, the last lines of `log.txt` name the exception:

- `EXCEPTION_ACCESS_VIOLATION` - a bad pointer. Please report the log.
- `STATUS_STACK_BUFFER_OVERRUN`, `STATUS_HEAP_CORRUPTION` - memory corruption.
  Please report the log.
- `UNHANDLED_CXX_EXCEPTION` - the exception message is in the log just above it.

The stack trace that follows lists the loaded modules; entries marked
`(filename not available)` are nearest-symbol guesses and are not reliable call
sites.

## The log is hard to read

Logging is serialized: each line is written in one piece, and the file is flushed
after every line so the last message before a crash is not lost. Third-party
libraries that write to the console directly (`Table Already Loaded`,
`Section closed, Record Count`) can still land between two entries.

Repeated table loads are cached, which removes most of that output.

## Saving a modern tile: "[ModernADT][Save]" messages

Modern (split-ADT) saves go through the native ROOT writer first
(`ModernAdtWriter`), then convert TEX0/OBJ0/OBJ1, then **validate every part
before anything is written**. A healthy save logs:

``` text
[ModernADT][Save] Saved split tile ROOT/TEX0/OBJ0/OBJ1/LOD to the project override: world/maps/...
```

What the other messages mean:

* `Native ROOT writer failed for "..."; falling back to the conversion path.`
  — the native writer hit a structural surprise in this tile's original
  files. The save still happens through the older conversion path (which now
  also repairs MCIN/MHDR bookkeeping). Worth reporting with the log attached.
* `Validation rejected the tile "...": <problems>` — pre-write validation
  found the assembled files inconsistent (the message lists every problem,
  e.g. an MCIN entry that does not match its MCNK). **Nothing was written**;
  your project files are untouched. This protects against corrupt map files:
  retry once, and if it repeats, report it with the log — the listed problems
  say exactly which part and chunk disagreed.
* `Split ADT save failed for "..."` — the fallback path failed too (or the
  original tile could not be loaded). Again, nothing was written.
* `Could not load the original split tile before saving` — the tile's
  original ROOT/TEX0/OBJ0 could not be read from the client; the save is
  skipped because preservation of unknown data would be impossible.

Details and the design are documented in
[`MODERN_ADT_WRITER.md`](MODERN_ADT_WRITER.md).

## Modern map metadata: "[WDT Load]" / "[WDT Save]" / "[WDL Load]" / "[WDL Save]" messages

Noggit now reads `.wdt` and `.wdl` files with a version-aware chunk walker
instead of the old fixed-order reader, and it validates every chunk it looks
at. All findings are tagged, with the map file name, so you can grep the log:

``` text
[WDT Load] World\Maps\TwilightHighlands\TwilightHighlands.wdt: skipped 1 known
but unused chunk(s) (MANM x1)
[WDL Load] "World\Maps\TwilightHighlands\TwilightHighlands.wdl": loaded 41
horizon WMO placement(s) from MLMD (17 unique model(s))
[WDL Save] Patched 212 MARE tile(s) in the modern WDL "..."; all other chunks
preserved byte for byte.
```

`[Debug]`-level lines (skipped/unknown chunk summaries, resolved FileDataIDs)
are informational. The lines that matter:

- `global WMO FileDataID N has no listfile entry` - the WDT announces a global
  WMO (WMO-only map) whose FileDataID your listfile cannot name. Check that
  the project's `listfile.csv` is complete for your client build.
- `MPHD flag 0x200 ... is set but the WDT has no MAID chunk` - the map wants
  FileDataID-based ADT loading but carries no MAID table; Noggit falls back
  to name-based lookups.
- `N tile(s) flagged as existing in MAIN have no root ADT FileDataID in MAID`
  - usually tiles created inside Noggit. They work in Noggit, but the modern
  client needs MAID registration before it can see them (see
  [MODERN_MAP_REGISTRATION.md](MODERN_MAP_REGISTRATION.md)).
- `WDT did not parse cleanly; continuing with the recovered MPHD/MAIN data`
  and `in-place patching failed; falling back to the legacy WDT writer` - the
  source file is corrupt or truncated. Saving will rebuild a basic WDT and
  modern-only chunks (MAID/MANM/...) will be lost; restore the file from a
  backup when you can.
- `[WDL Save] N horizon tile(s) have no MARE slot in the source WDL` - the
  map gained tiles that the original WDL has no height slot for; their
  horizon heights cannot be persisted without rebuilding the whole file,
  which would destroy the modern chunks, so Noggit keeps the file intact.
- `MAOF/MARE/MAHO ... unexpected size ... tile skipped` - the WDL height data
  is damaged; the affected tiles fall back to the tile-index minimap.

Modern saves never rebuild these files from scratch: the loaded image is
patched in place (MPHD flags + MAIN tile flags for the WDT, MARE/MAHO height
payloads for the WDL), so chunks Noggit does not understand survive saving
byte for byte. Full details in [MODERN_WDT_WDL.md](MODERN_WDT_WDL.md).

## Loading modern tiles: "[ModernADT]" messages

Every message about Shadowlands/retail ADT loading carries the
`[ModernADT]` tag (UID fixing uses `[UIDFix]`, object attachment adds
`[Objects]`, texture decoding adds `[TexDiag]`). Errors mean data was
lost or a file was rejected; warnings and debug lines are informational.
The most common ones:

* `Refusing to load '...': its ROOT part is not a usable modern or legacy
  ADT` - the root ADT is missing, truncated or has no recognisable
  layout. The messages directly above name the exact chunk problem. The
  tile is skipped; the rest of the map still loads.
* `is a legacy single-file ADT ... routing it to the legacy loader` -
  normal when a WotLK-style ADT lives inside a modern project (for
  example a tile you converted yourself). Nothing is lost.
* `the tile will load without terrain textures` / `without model/WMO
  placements` - the `_tex0` / `_obj0` part is missing or damaged. The
  terrain itself still loads and stays editable; re-saving the tile
  rebuilds the missing part from what Noggit has.
* `N/256 chunk(s) failed to load (first error: ...); flat placeholders
  were inserted` - individual damaged terrain chunks were replaced by
  flat placeholders so the rest of the tile stays usable. Re-save the
  tile to write a clean file, or sculpt the placeholders back.
* `MHDR flag 0x1 announces MFBO flight bounds but no valid MFBO chunk was
  found` - the file claims flight bounds but the chunk is missing or
  malformed; bounds stay empty. Cosmetic unless you rely on camera
  clipping.

For a deep look at what Noggit sees inside one tile, enable verbose
per-chunk diagnostics (first 4 chunks of every tile: height tables and
TEX0 layer dumps) with the setting:

    modern_adt_verbose_diagnostics = true

in `noggit.ini` (or the Qt settings registry), then restart Noggit.
Full details of the loader, the shared reader module and the failure
behaviours are in [MODERN_ADT_LOADING.md](MODERN_ADT_LOADING.md).

## A map won't open: wall of "Uncaught exception ... Paint event" errors

Symptom: after picking a map, the view stays empty/frozen and `log.txt`
fills with thousands of lines like:

    [Error] Uncaught exception while handling a Paint event for MapView:
    File read failed. requested={path=world/maps/<map>/<map>_X_Y_obj0.adt, ...}

or `... Paint event for MapView: unknown exception`, repeating every
frame.

Cause: while the map view initialises, Noggit scans every tile's object
data (the `_obj0` part on modern maps) to find the highest stored GUID.
`exists()` asks the listfile/CASC metadata, which can advertise a file
whose *content* the CASC installation cannot deliver - typical for
partially downloaded clients ("metadata-less CASC fallback" near the top
of the log) or custom maps whose injection is incomplete or whose
listfile is stale. The failed read used to escape as an exception,
aborting GL initialisation; every repaint retried it, producing the
storm.

This is fixed in Noggit (the scan skips unreadable files with one
`getHighestGUIDFromFile(): could not read ...` error each, and the whole
UID phase can no longer abort the map view - the map opens anyway). If
you still see many missing files, the underlying data problem remains:

* `File world/wmo/... .wmo could not be loaded`, `... .m2`, `... .blp` -
  those assets are absent from your CASC installation; the objects are
  skipped and the rest of the map works. Repair/re-download the client
  (Battle.net -> Scan and Repair) to get the content back.
* For custom maps: verify the injected files (root/`_tex0`/`_obj0`) are
  really present under the FileDataIDs your listfile claims.

## Walls of `context.inl` GL errors (e.g. `bindBuffer ... GL_INVALID_OPERATION`)

A single stale or never-created GPU object (typically a buffer whose name
became invalid after a graphics-context change) makes the same OpenGL call
fail on **every frame**, so one broken object used to produce tens of
thousands of identical `[Error]` lines (observed: 46k+ `bindBuffer
GL_INVALID_OPERATION` entries in one session). The log is now aggregated:

- The **first** occurrence of each distinct GL error is logged in full.
- While the same error keeps repeating, at most one summary line per five
  seconds is written: `... (+ N identical repeats since the last report ...)`.
- The wrappers that failed in practice (`bindBuffer`, `bindVertexArray`,
  `drawElements`, `drawElementsInstanced`, `texParameteri`, `bufferData`,
  `bufferSubData`, and the scoped `vao_binder` / `buffer_binder` helpers) now
  record the calling site, so reports end with
  `| called from <file>:<line> in <function>`.

If you see such a wall, look at the **first** error line and its
`called from` location - that names the code path drawing with the broken
object - and report it together with what you did right before the errors
started (opening a map, switching maps, docking/undocking panels, changing
monitors or resolution). Context switches are the usual trigger.

## Reporting a problem

Include `log.txt`, the project's client version (Shadowlands 9.2.7, Retail,
Forever), the client build Noggit detected (`Detected modern client build: ...`
near the top of the log), and what you did last. The first 40 lines of the log
identify the Noggit version, the graphics driver, and the project.

## Legacy tables on 9.2.7: the definitions-supplement directory

The pinned ShadowWorld definitions set does not carry the Shadowlands-era
layouts of a few legacy tables (MapDifficulty, SoundKit,
SoundProviderPreferences, TerrainType, ZoneMusic, ZoneIntroMusicTable,
WindSettings, WeatherXParticulate, ZoneLightPoint, LiquidMaterial). Their
9.2.7 client files are WDC3, whose header `field_count` only counts the
columns packed into the record; the complete column list - the one a `.dbd`
block describes - is `total_field_count`, and the definition fallback now
compares against that.

`dist/definitions-supplement/` carries the canonical wowdev/WoWDBDefs blocks
for exactly those tables and is copied next to the executable as
`definitions-supplement/` at build time. When no block in the primary
definitions describes a client file, the fallback consults that sibling
directory and still accepts a block only when it declares the file's layout
hash or its exact column count, so a wrong definition can never slip in.
The log names the source that won:

    [DB2Definitions] MapDifficulty.db2: ... using the 9.2.7.45745 definition
    from definitions-supplement instead (layout 43E2A417, ... records)
