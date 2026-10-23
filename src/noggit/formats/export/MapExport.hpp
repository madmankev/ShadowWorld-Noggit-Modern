#pragma once

//! \brief Packaging step from saved project files to a deployable,
//! client-ready map set: the "export" half of native modern map writing.
//!
//! Loose files in the project folder are what the *editor* reloads; a game
//! client needs more than that. Three gaps the tree's own docs named are
//! what this module closes:
//!
//!  * "New tiles have no MAID FileDataIDs ... allocating ids and writing them
//!    belongs to the map-registration/export pipeline" (MODERN_WDT_WDL.md).
//!    A modern client locates every ADT through the WDT's MAID table, so a
//!    tile created in Noggit with zeroed MAID entries is a tile the client
//!    cannot see. exportModernMap fills those entries for the tiles the
//!    export set actually contains, and says so in the artifacts.
//!  * "producing CASC-visible fds for new maps belongs to a future export
//!    step" (MODERN_MAP_REGISTRATION.md). The export assigns the WDT (and
//!    WDL, and any MPHD-referenced aux WDT the client does not know) a
//!    FileDataID and emits the registration fields - WdtFileDataID included -
//!    for extraData/modernMapRegistrations.json.
//!  * "there is no packaging/export pipeline in this tree" (same doc). The
//!    export validates the *whole* set the way the save path validates a
//!    single tile - inventory against MAIN, structural validation of every
//!    ADT part through ModernAdtWriter::validatePart, MPHD/MAID consistency -
//!    and produces the deployable file set plus three artifacts:
//!    listfile additions (fdid;path), the registration fields, and a
//!    manifest following the conversion_manifest.json conventions.
//!
//! Ids are only invented where an id is all that is missing: the file bytes
//! must be in the export set. Nothing that needs real content (MAI2, sky
//! scenes, baked map textures) is fabricated; what cannot be produced
//! offline is reported instead.
//!
//! The export never mutates the project's own WDT on disk: the patched WDT
//! goes into the export set, and the project keeps the client original the
//! save path's self-heal protects.
//!
//! Qt-free and dependency-free so the whole pipeline can be unit tested.

#include <noggit/formats/conversion/MapConversion.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Noggit::Formats::Export
{
  struct ExportFile
  {
    //! Client-relative path, e.g.
    //! "World\Maps\Test\Test_12_34_tex0.adt". Separators and case are
    //! normalized internally (lowercase, backslashes), matching the
    //! listfile/ClientData convention.
    std::string relative_path;
    std::vector<std::uint8_t> bytes;
  };

  struct ExportOptions
  {
    //! WDT directory name of the map, e.g. "test".
    std::string map_directory;
    //! Display name for the registration's MapName_lang; falls back to the
    //! directory name when empty.
    std::string map_display_name;
    //! Map.db2 id when known (-1 leaves ID out of the registration fields).
    int map_id = -1;
    std::string target_build = "9.2.7.45745";
    //! First FileDataID handed to files the client does not know - the same
    //! convention (and default base) the map converter uses.
    std::uint32_t fdid_base = 900000000u;
  };

  struct ExportInput
  {
    //! Everything the project holds for this map: ADT parts (all five where
    //! they exist), the WDT, the WDL, the aux WDTs.
    std::vector<ExportFile> files;
    //! The FileDataID the client's listfile already resolves for a
    //! normalized path; 0 when the client does not know the file. May be
    //! empty (then every needed id is treated as unregistered).
    std::function<std::uint32_t(std::string const&)> path_to_fdid;
  };

  struct ExportResult
  {
    //! False when anything is missing or invalid: `errors` says exactly what.
    //! Warnings do not fail the export.
    bool ok = false;
    //! The deployable set: every input file, with the WDT replaced by its
    //! MAID/MPHD-assigned version.
    std::vector<ExportFile> files;
    //! Every FileDataID the target client has to learn about (listfile and,
    //! server-side, CASC registration). existed_in_client == false entries
    //! are the ones this export allocated.
    std::vector<Conversion::FdidAssignment> fdid_assignments;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    unsigned int tiles_total = 0;
    unsigned int tiles_complete = 0;

    //! Same conventions as conversion_manifest.json: files with sizes and a
    //! content hash, fdid assignments, registration fields, warnings/errors.
    std::string manifest_json() const;
    //! "fdid;path" lines for every newly allocated id - the piece a CASC
    //! rebuild / server pack needs to make the files resolvable.
    std::string listfile_csv() const;
    //! Map.db2 column values for extraData/modernMapRegistrations.json
    //! (Directory, MapName_lang, WdtFileDataID, ...). A fragment to merge,
    //! not a full registration row.
    std::map<std::string, std::string> registration_fields() const;

    //! Echo of the options the export ran with (manifest uses them).
    ExportOptions options;
    //! WDT fdid the registration should carry (allocated or known), 0 = none.
    std::uint32_t wdt_fdid = 0;
  };

  //! Run the export. Never throws for data problems: everything is reported
  //! through the result (ok == false + errors on failure).
  ExportResult exportModernMap(ExportOptions const& options, ExportInput const& input);
}
