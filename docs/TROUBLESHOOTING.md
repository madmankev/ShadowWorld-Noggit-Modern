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
