# Modern Map.db2 registration for new maps

Status: implemented for Shadowlands 9.2.7 (45745) and later modern projects
(`ProjectVersion::SL`, `RETAIL`, `FOREVER`). Legacy projects are unchanged.

Upstream issue: *Add Map.db2 registration for new maps* — support registering
newly created maps into the appropriate modern `Map.db2` data instead of
relying solely on the legacy map registration workflow.

## The problem this solves

On legacy projects the map creation wizard persists a new map by rewriting
`<project>/DBFilesClient/Map.dbc`, which the editor reads back on the next
load. On modern projects that file is dead data:

1. The client's map data lives in `Map.db2` inside CASC, which is read-only.
2. At project load, the modern DB2 bridge (`ShadowlandsMapDB2Bridge`)
   materializes the client's `Map.db2` rows into the editor's legacy-shaped
   map table and **overwrites** it wholesale (`gMapDB.overwriteWith(...)`).

So a map created through the wizard existed for the current session only and
vanished on restart.

## The design

Registrations are stored project-side, in modern column terms, and merged by
the bridge:

```
<project>/extraData/modernMapRegistrations.json
```

```json
{
  "version": 1,
  "maps": [
    {
      "id": 2100,
      "target_build": "9.2.7.45745",
      "fields": {
        "Directory": "New_Map",
        "InstanceType": "0",
        "Flags": "0",
        "MapName_lang": "New Map",
        "AreaTableID": "0",
        "MapDescription1_lang": "",
        "MapDescription0_lang": "",
        "LoadingScreenID": "0",
        "MinimapIconScale": "1.000000",
        "CorpseMapID": "-1",
        "TimeOfDayOverride": "-1",
        "ExpansionID": "8",
        "RaidOffset": "0",
        "MaxPlayers": "0"
      },
      "array_fields": { "Corpse": ["0.000000", "0.000000"] }
    }
  ]
}
```

* **Wizard side** (`MapCreationWizard::registerMapForModernPipeline`): when a
  map is saved in a modern project, the wizard writes the new map's values
  under their **modern `Map.db2` column names**, records the client build it
  was authored against (`target_build`), validates the entry, and upserts it
  into the JSON above. Deleting a map removes its registration
  (`unregisterMapFromModernPipeline`).
* **Bridge side** (`merge_registered_maps` in `ShadowlandsMapDB2Bridge.cpp`):
  after the CASC rows are converted, registrations are validated again and
  applied to the bridged table with the exact same modern-column → legacy-row
  mapping the CASC converter uses. **Client data wins on id conflicts** (a
  registration whose id exists in `Map.db2` is skipped with a debug note).
* The in-session behaviour is unchanged: the wizard still adds the record to
  the live table immediately, so the map is usable without a restart.

Storage, validation and application live in
`src/noggit/project/ModernMapRegistrations.{h,cpp}` — deliberately Qt-free and
dependency-free (small hand-rolled JSON codec for this one schema) so the
paths can be unit-tested outside the application.

## Required fields for the first target build (Shadowlands 9.2.7, 45745)

The 9.2.7 `Map` layout is `LAYOUT AAA1F94E` (24 columns) in
`dist/definitions/Map.dbd`:

```
ID, Directory, MapName_lang, MapDescription0_lang, MapDescription1_lang,
PvpShortDescription_lang, PvpLongDescription_lang, Corpse[2], MapType<u8>,
InstanceType<8>, ExpansionID<u8>, AreaTableID<u16>, LoadingScreenID<16>,
TimeOfDayOverride<16>, ParentMapID<16>, CosmeticParentMapID<16>,
TimeOffset<u8>, MinimapIconScale, CorpseMapID<16>, MaxPlayers<u8>,
WindSettingsID<16>, ZmpFileDataID<32>, WdtFileDataID<32>, Flags<32>[3]
```

Notes on this layout:

* `Flags` is an **array of 3**; the bridge reads the first element (falling
  back to scalar reads on other layouts).
* There is **no `RaidOffset` column** — it is a legacy-only field. The wizard
  still stores it (the legacy record keeps it), and validation treats it as
  optional.
* `MapType<u8>` is distinct from `InstanceType`; the editor's legacy table has
  a single `AreaType` column which the bridge fills from `InstanceType`.

Validation (`ModernMapRegistrations::validate`) requires the columns a map
cannot function without:

| Field | Rule |
| --- | --- |
| `Directory` | non-empty, ≤ 64 chars, `[A-Za-z0-9_]` only (it becomes a file path) |
| `MapName_lang` | non-empty |
| `InstanceType` | integer, 0 (none) … 4 (arena) |
| `Flags` | non-negative integer |
| `AreaTableID`, `LoadingScreenID`, `MaxPlayers`, `ExpansionID` | non-negative integers (`ExpansionID` ≤ 20) |
| `MinimapIconScale` | number |

Everything else (`Corpse`, `CorpseMapID`, `TimeOfDayOverride`,
`MapDescription*_lang`, …) is optional and falls back to converter defaults.
Numeric checks are strict (whole-string parses; no negatives where unsigned);
malformed registrations are skipped with a `LogError` and never abort the
bootstrap.

## Version awareness

* A registration stores the build it was authored for (`target_build`), so
  future client generations can tell where a value came from.
* Merging is by **column name**, not binary layout: applying a 9.2.7-era
  registration on a later build simply leaves that build's new columns at the
  bridge defaults, and columns a registration does not carry (e.g. a future
  build dropping one) are ignored.
* The JSON file carries a `version` marker; files from a newer format are
  reported with a `LogError` instead of being silently misread.
* The acceptance logic deliberately does **not** require the registration's
  target build to match the loaded client build — a mismatch logs nothing
  per-map and merges with defaults, which is the desired forward behaviour.

## What is deliberately out of scope (follow-ups)

* **Writing a real `Map.db2`**: Noggit edits against a read-only CASC client;
  there is no packaging/export pipeline in this tree to receive one. The
  registration format keeps modern column values so an exporter can be added
  later without changing stored data.
* **`WdtFileDataID` / `ZmpFileDataID`**: 9.2.7 references WDT/ZMP by file
  data id. The editor resolves WDT by `Directory` path (project files first),
  so registration works editor-side; producing CASC-visible fds for new maps
  belongs to a future export step.
* **AreaTable registration** for zones on new maps (the wizard only registers
  the map row itself).

## Testing

Unit-tested outside the app against stubbed Qt-free headers (JSON round-trip
with UTF-8/escapes, corrupt + newer-format files, id-less entries, upsert/
remove, 15 validation cases, legacy-record mapping incl. defaults, and
client-wins conflict handling). To re-run: compile
`src/noggit/project/ModernMapRegistrations.cpp` with a test main against stub
`noggit/Log.h`, `noggit/DBC.h`, `noggit/DBCFile.h`.

In-app verification (9.2.7 project): create a map in the wizard, check for
`[MapWizard] Registered map id=... in extraData/modernMapRegistrations.json`,
restart the editor, and confirm `[ModernDB2][Map] Map registrations merged=1`
during bootstrap and the map in the map list.
