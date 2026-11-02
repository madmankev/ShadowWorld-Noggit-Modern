# Forever (1.60.x) client support

ShadowWorld's Forever beta clients (build `1.60.x`) are supported as their own
project version. This page records how the editor treats them and why, based on
field evidence from a `1.60.1.70685` client (its own CASC files: `Kalimdor.wdt`
carries **MAI2**, `Kalimdor.wdl` carries MLDD/MLDF/MLDX and **no MSSF**).

## Project version and definitions

- A project created with version **Forever** opens the client CASC with the
  Forever archive profile and prefers `.db2` database files.
- The editor looks for an optional `definitions-forever/` directory next to the
  executable for Forever-specific `.dbd` definitions. It does not ship with the
  installation. When it is absent, the shipped `definitions/` directory is used
  instead: its nearest-build fallback covers 1.60.x clients (a 1.60.1.70685
  client bootstraps `AreaTable`, `Light`, `LightParams`, `LightData`,
  `LightSkybox`, ... through the 11.2.5.64502 definitions). To supply your own
  Forever definitions, create `definitions-forever/` with `<TableName>.dbd`
  files; it is then used automatically.

## Build detection

`detectClientBuildVersion` resolves the client's real build from:

1. `<client>/.build.info`, then `<client>/_retail_/.build.info`, then the
   **install root's** `.build.info` (Battle.net keeps one shared file there
   with a row per product). Rows are matched to the selected folder by the
   `Product install path!` column, then by `.flavor.info` product code, then
   the `Active!` row, then the first row.
2. `Data/config/**` build configurations (`WOW-<build>patch<x.y.z>`), walking
   up to the install root when the product folder has none.

If detection fails entirely the project falls back to `9.2.7.45745` and says so
with an error line; recreate the project after fixing the client path rather
than continuing on the fallback build.

## Version/build cross-check

At load time the detected build is cross-checked against the project version
(Forever = 1.x, Shadowlands = 9.x, Retail = 10.x+). A mismatch logs an error
explaining that definitions and the CASC profile follow the *project version*,
so a mislabeled project is visible immediately instead of failing silently
later.

## Map-file generation

Forever is a hybrid of the two modern families:

| Feature            | Shadowlands 9.2.7 | Forever 1.60.x | Retail 12.x |
|--------------------|-------------------|----------------|-------------|
| Split ADTs         | yes               | yes            | yes         |
| WDT `MAI2`         | no                | **yes**        | yes         |
| WDL `MSSF`         | no                | **no**         | yes         |
| WDL sky scenes     | yes               | yes            | yes         |

Consequences for `Export map for client...` generation stepping:

- **Retail -> Forever** keeps `MAI2` in the WDT and strips `MSSF` from the WDL.
- **Shadowlands -> Forever** cannot invent `MAI2` (it is a per-tile FileDataID
  table); the WDT keeps its bytes and the report notes the honest absence.
  Take `MAI2` from the Forever client's own WDT where one is available.
- **Forever -> Shadowlands** strips `MAI2` and reports it.
- **Forever -> Retail** is an identity for WDTs that already carry `MAI2`.

ADT parts are identical across all modern families and cross verbatim.

When a Forever project opens a map whose WDT was authored for another
generation (e.g. a 9.2.7 project override), the `[WDT Load]` advisory names
both generations. Saves keep the client's own WDT; only the export steps
map-level files between generations.
