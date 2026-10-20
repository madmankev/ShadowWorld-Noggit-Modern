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
contain chunks that Noggit does not need: M2 and WMO placements (`MLDD`, `MLDX`,
`MLMD`, `MLMX`), sky scenes (`MSSN`, `MSSC`, `MSSO`, `MSSF`), ocean and file-data
chunks (`MAOE`, `MAID`, `MWAT`). They are skipped and reported once per file at
`[Debug]` level:

``` text
wdl "World\Maps\TwilightHighlands\TwilightHighlands.wdl": skipped 492 unsupported
chunks (MLDD x123, MLDX x123, MLMD x123, MLMX x123)
```

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
