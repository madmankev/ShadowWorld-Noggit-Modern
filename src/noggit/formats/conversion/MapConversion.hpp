#pragma once
// Two-way WotLK <-> modern (Shadowlands 9.2.7) map conversion.
//
// Explicit, file-to-file conversion between the two map file families the
// editor supports (see docs/MAP_CONVERSION.md). The core is Qt-free and
// logging-free: file access and FileDataID resolution are injected through
// ConversionFileSource callbacks, so the whole converter can be exercised
// by the standalone harness.
//
// convertMap() orchestrates a whole map (WDT + WDL + every tile with an
// ADT); the piece functions below it are exposed for unit tests and for
// callers that only need part of the pipeline.

#include <array>
#include <cstdint>
#include <noggit/MapHeaders.h>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Formats::Conversion
{

  //! Bring a WMO placement (a MODF entry) into a shape the client can consume.
  //!
  //! Entries whose extents are non-finite or inverted - lower corner above the
  //! upper corner - make the client's spatial queries walk garbage memory, and
  //! both hand-edited projects and legacy imports contain them, while the split
  //! pipeline copies MODF entries verbatim into OBJ0. Non-finite extents become
  //! a one-unit box around the placement position, inverted corners are swapped
  //! per axis, and non-finite positions or rotations become zero.
  void sanitizeModfEntry(ENTRY_MODF& entry);

  enum class MapFormat
  {
    wotlk,   //!< 3.3.5: monolithic ADT, name-referenced objects, MCLQ
    modern   //!< split ADT parts, FileDataID references, MH2O; which modern
             //!  family is meant by ConversionOptions::target_generation
  };

  //! The post-WotLK client families, as distinct formats rather than one
  //! generic "modern".
  //!
  //! A Shadowlands client and a current retail client agree on the ADT layout
  //! (MVER 18) but not on the map-level file set: the WDT carries MAI2 since
  //! 12.0.5 and the WDL carries MSSF since Dragonflight, and their listfiles
  //! resolve different FileDataIDs for the same path. Naming the generation is
  //! what makes conversion between generations expressible, and what keeps a
  //! save aimed at one client from silently writing the other's file set.
  enum class MapGeneration
  {
    wotlk_3_3_5,
    shadowlands_9_2_7,
    retail_12_x,
  };

  struct GenerationFeatures
  {
    MapGeneration generation;
    char const* label;  //!< "3.3.5", "9.2.7", "12.x"
    bool split_adt;     //!< modern families; WotLK is monolithic
    bool wdt_mai2;      //!< WDT MAI2, 12.0.5+
    bool wdl_mssf;      //!< WDL MSSF, Dragonflight+
    bool wdl_mssn;      //!< WDL sky scenes, Shadowlands+ (both modern families)
  };

  GenerationFeatures const& generationFeatures(MapGeneration generation);

  //! Which modern family a map-level file set belongs to, from the chunks it
  //! actually carries: MAI2 in the WDT or MSSF in the WDL means retail, their
  //! absence means Shadowlands. Falls back to Shadowlands when neither file
  //! walks, which is the family the editor saves in.
  MapGeneration detectModernGeneration(std::vector<std::uint8_t> const& wdt,
                                      std::vector<std::uint8_t> const& wdl);

  //! The result of moving one file between generations.
  struct GenerationConversionReport
  {
    bool ok = false;
    std::vector<std::uint8_t> bytes;
    //! What happened, per chunk: stripped because the target family does not
    //! carry it, or left absent because authoring it offline would mean
    //! fabricating content (per-tile FileDataIDs, sky scenes) the target client
    //! cannot resolve.
    std::vector<std::string> notes;
  };

  //! Convert a WDT between post-WotLK generations. Chunks the target family
  //! does not carry are stripped byte-exactly; chunks it carries but the source
  //! lacks are reported and left absent, never invented.
  GenerationConversionReport convertWdtBetweenGenerations(
      std::vector<std::uint8_t> const& source, MapGeneration from, MapGeneration to);

  //! The WDL counterpart of convertWdtBetweenGenerations.
  GenerationConversionReport convertWdlBetweenGenerations(
      std::vector<std::uint8_t> const& source, MapGeneration from, MapGeneration to);

  //! The ADT layout is identical across the modern families, so between them an
  //! ADT part converts by validation and verbatim copy: the part must walk
  //! clean and carry MVER 18, and is then unchanged. WotLK on either end is a
  //! different conversion, not a generation step, and is refused here.
  GenerationConversionReport convertAdtPartBetweenGenerations(
      std::vector<std::uint8_t> const& source, MapGeneration from, MapGeneration to);

  //! Everything the converter needs from the application: reading source
  //! files and translating between paths and FileDataIDs.
  struct ConversionFileSource
  {
    //! Read a source file by normalized relative path (lowercase, forward
    //! slashes), e.g. "world/maps/azeroth/azeroth_12_34.adt".
    //! nullopt when the file does not exist.
    std::function<std::optional<std::vector<std::uint8_t>>(std::string const&)> read_file;
    //! Normalized path -> FileDataID known to the target client's listfile.
    //! nullopt when unknown (the converter then allocates a custom id).
    std::function<std::optional<std::uint32_t>(std::string const&)> path_to_fdid;
    //! FileDataID -> path from the source client's listfile; empty string
    //! when unknown.
    std::function<std::string(std::uint32_t)> fdid_to_path;
  };

  struct ConversionOptions
  {
    MapFormat target = MapFormat::modern;
    //! Directory/base name of the map, e.g. "Azeroth" for
    //! World/Maps/Azeroth/Azeroth.wdt.
    std::string map_name;
    //! Map.db2/Map.dbc id, used for the suggested registration. -1 = keep
    //! whatever the caller fills in later.
    int map_id = -1;
    //! Client build the conversion targets (registration metadata).
    std::string target_build = "9.2.7.45745";
    //! Which modern family MapFormat::modern means here. Defaults to the
    //! family the editor saves in; set retail_12_x to convert a map up to a
    //! current retail client, which adds the WDT/WDL chunks that family
    //! carries where they can be had without fabricating content, and reports
    //! the rest.
    MapGeneration target_generation = MapGeneration::shadowlands_9_2_7;
    //! First FileDataID handed to content the listfile cannot resolve and
    //! to the converted map files themselves. Kept far away from retail
    //! ids; must not collide with the server's own custom id range.
    std::uint32_t custom_fdid_base = 900000000u;
    //! Convert MCLQ <-> MH2O. When false, liquids are dropped going to
    //! WotLK and kept as-is going to modern (MH2O passthrough only).
    bool convert_liquids = true;
    //! Also convert <map>.wdl when present.
    bool include_wdl = true;
    //! Legacy WDT "big alpha" flag (small alpha is TBC-era; only relevant
    //! for the modern->WotLK direction).
    bool big_alpha = true;
    //! Map mclq magma to LiquidType 15 (Green Lava) instead of 3.
    bool green_lava = false;
  };

  //! One FileDataID the target client has to know about (listfile entry
  //! and/or server-side registration).
  struct FdidAssignment
  {
    std::uint32_t fdid = 0;
    //! Normalized path of the referenced file.
    std::string path;
    //! "texture", "m2", "wmo", "wdt", "wdl", "adt_root", "adt_tex0",
    //! "adt_obj0".
    std::string role;
    //! True when the id came from the client's listfile, false when the
    //! converter allocated it (those need registration).
    bool existed_in_client = false;
  };

  struct ConvertedFile
  {
    //! Target-relative path with forward slashes, e.g.
    //! "World/Maps/Azeroth/Azeroth_12_34_tex0.adt".
    std::string relative_path;
    std::vector<std::uint8_t> bytes;
  };

  struct ConversionReport
  {
    bool ok = false;
    MapFormat source = MapFormat::wotlk;
    MapFormat target = MapFormat::modern;
    //! Echoed from the options for the manifest/UI.
    std::string map_name;
    int map_id = -1;
    std::string target_build;
    std::vector<ConvertedFile> files;
    std::vector<FdidAssignment> fdid_assignments;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    unsigned tiles_total = 0;
    unsigned tiles_converted = 0;
    unsigned tiles_skipped = 0;
    //! Suggested Map.db2 registration for the converted map, in the field
    //! shape of extraData/modernMapRegistrations.json (empty when the
    //! source carries no usable id/parent information).
    std::map<std::string, std::string> suggested_registration_fields;
    std::map<std::string, std::vector<std::string>> suggested_registration_array_fields;

    //! Manifest text (JSON) describing the conversion: options, files,
    //! fdid assignments, registration suggestion, warnings. Written next
    //! to the converted output by the caller.
    std::string manifest_json() const;
  };

  //! Convert a whole map. Never throws for data problems: everything is
  //! reported through ConversionReport (ok == false + errors on failure).
  ConversionReport convertMap(ConversionOptions const& options,
                              ConversionFileSource const& source);

  // ------------------------------------------------------------------
  // Pieces (public for the harness and for partial pipelines)
  // ------------------------------------------------------------------

  struct ConversionIssue
  {
    enum class Severity { warning, error };
    Severity severity = Severity::warning;
    std::string message;
  };

  //! Allocate custom FileDataIDs sequentially, recording what they were
  //! handed out for. Ids are cached per path: the same file referenced by
  //! many tiles always gets the same id, matching how the client listfile
  //! works.
  class FdidAllocator
  {
  public:
    explicit FdidAllocator(std::uint32_t base);
    //! Resolve a path: prefer the id the client listfile already knows
    //! (through path_to_fdid), else allocate the next custom id.
    std::uint32_t resolve(std::string const& raw_path,
                          std::string const& role,
                          ConversionFileSource const& source);
    std::vector<FdidAssignment> const& assignments() const { return _assignments; }
    void record_existing(std::uint32_t fdid, std::string path, std::string role);
  private:
    std::uint32_t allocate(std::string path, std::string role);
    std::uint32_t _next;
    std::vector<FdidAssignment> _assignments;
    std::map<std::string, std::uint32_t> _by_path;
  };

  std::string normalize_path(std::string const& path);

  //! Result of converting one tile's ADT files.
  struct AdtConversionResult
  {
    bool ok = false;
    //! Modern direction: root/tex0/obj0 bytes. Legacy direction: single
    //! monolithic adt in `root`.
    std::vector<std::uint8_t> root;
    std::vector<std::uint8_t> tex0;
    std::vector<std::uint8_t> obj0;
    std::vector<ConversionIssue> issues;
  };

  //! WotLK monolithic ADT -> modern ROOT/TEX0/OBJ0 part bytes.
  AdtConversionResult convertAdtLegacyToModern(
    std::vector<std::uint8_t> const& legacy_adt,
    ConversionOptions const& options,
    ConversionFileSource const& source,
    FdidAllocator& fdids);

  //! Modern ROOT/TEX0/OBJ0 part bytes -> WotLK monolithic ADT in `root`.
  //! obj1/lod bytes may be empty; they are only inspected for the drop
  //! warnings.
  AdtConversionResult convertAdtModernToLegacy(
    std::vector<std::uint8_t> const& root,
    std::vector<std::uint8_t> const& tex0,
    std::vector<std::uint8_t> const& obj0,
    std::vector<std::uint8_t> const& obj1,
    ConversionOptions const& options,
    ConversionFileSource const& source,
    FdidAllocator& fdids);

  //! Legacy WDT bytes -> modern WDT bytes. `tile_fdids` maps tile index
  //! (y * 64 + x) to the MAID entry for the converted tile files (all
  //! zeros for tiles without ADT).
  struct MaidEntryOut
  {
    std::uint32_t root_adt = 0;
    std::uint32_t obj0 = 0;
    std::uint32_t obj1 = 0;
    std::uint32_t tex0 = 0;
    std::uint32_t lod = 0;
    std::uint32_t map_texture = 0;
    std::uint32_t map_texture_n = 0;
    std::uint32_t minimap = 0;
  };

  std::vector<std::uint8_t> buildModernWdt(
    std::vector<std::uint8_t> const& legacy_wdt,
    std::array<MaidEntryOut, 4096> const& tile_fdids,
    ConversionFileSource const& source,
    FdidAllocator& fdids,
    std::vector<ConversionIssue>& issues);

  std::vector<std::uint8_t> buildLegacyWdt(
    std::vector<std::uint8_t> const& modern_wdt,
    ConversionOptions const& options,
    ConversionFileSource const& source,
    FdidAllocator& fdids,
    std::vector<ConversionIssue>& issues);

  //! Legacy WDL bytes -> modern WDL bytes (MWMO/MWID/MODF -> MLMD/MLMX,
  //! MAOF/MARE/MAHO passthrough).
  std::vector<std::uint8_t> buildModernWdl(
    std::vector<std::uint8_t> const& legacy_wdl,
    ConversionFileSource const& source,
    FdidAllocator& fdids,
    std::vector<ConversionIssue>& issues);

  //! Modern WDL bytes -> legacy WDL bytes (MLMD/MLMX -> MWMO/MWID/MODF,
  //! MLDD/MLDX dropped with a warning).
  std::vector<std::uint8_t> buildLegacyWdl(
    std::vector<std::uint8_t> const& modern_wdl,
    ConversionFileSource const& source,
    FdidAllocator& fdids,
    std::vector<ConversionIssue>& issues);
}
