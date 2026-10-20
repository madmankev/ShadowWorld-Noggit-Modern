// This file is part of Noggit3, licensed under GNU General Public License (version 3).

//! \brief Version-aware reader/validator for Shadowlands-era split ADT files.
//!
//! Modern (Legion+) ADTs are split into ROOT (heights, MCNK grid), _tex0
//! (MDID/MHID texture ids, MTXP parameters, per-chunk MCLY/MCAL), _obj0
//! (MDDF/MODF placements referencing models by FileDataID), _obj1 (LOD
//! placements) and _lod parts, while legacy (WotLK) ADTs are single files
//! carrying MTEX/MMDX/MWMO name lists next to the MCNK grid.
//!
//! Historically the loading pipeline mixed three weakly validated readers:
//! the AsyncLoader gate scan, the terrain loader in MapTileModern.cpp and
//! the OBJ0 attachment walker in AsyncObject.cpp - all assert()-assisted or
//! throw-on-first-problem, with per-MCNK debug dumps flooding the log. This
//! module consolidates the pure parsing/validation logic so it is shared,
//! testable without Qt, and reports problems as structured issues instead of
//! aborting whole tiles:
//!
//!   - scanAdt(): top-level chunk walk with layout markers (MVER/MHDR/MCIN/
//!     MTEX/.../MCNK), MHDR + MFBO extraction, MCNK offset table and
//!     optional per-MCNK subchunk validation.
//!   - classifyAdtLayout(): decides legacy single file vs modern split root
//!     vs unusable from the *file content*, not the project version.
//!   - parseTex0(): MDID/MHID/MTXP + 256 MCNK (MCLY/MCAL) decoding with
//!     FileDataID resolution tables and MTXP application.
//!   - decodeModernAlpha(): compressed/raw/4-bit MCAL expansion.
//!   - parseObj0(): MDDF/MODF placement extraction.
//!
//! No Qt, no logging: callers turn issues into log lines.

#pragma once

#include <noggit/Alphamap.hpp>
#include <noggit/MapHeaders.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Noggit::Formats::ADT
{
  enum class AdtIssueSeverity
  {
    info,
    warning,
    error
  };

  struct AdtIssue
  {
    AdtIssueSeverity severity = AdtIssueSeverity::info;
    std::string message;
  };

  //! File layout classification, derived from chunk content.
  enum class AdtLayout
  {
    //! Neither a usable legacy file nor a usable modern ROOT.
    unusable,
    //! WotLK-style single file: carries MTEX/MMDX/MWMO name lists.
    legacy_single_file,
    //! Legion+ ROOT part of a split ADT (heights + MCNK grid).
    modern_split_root
  };

  struct AdtScanOptions
  {
    //! ROOT MCNKs carry the 128-byte MapChunkHeader before their subchunks;
    //! TEX0/OBJ0 MCNKs are pure subchunk containers.
    bool mcnk_has_header = true;
    //! Walk every MCNK's subchunks and report anomalies.
    bool validate_mcnk_subchunks = true;
    //! Cap for per-MCNK anomaly messages (the total count is always reported).
    std::size_t max_subchunk_issue_messages = 8;
    //! Label used in issue messages ("ROOT", "TEX0", "OBJ0", ...).
    std::string label = "ADT";
  };

  struct MfboExtents
  {
    std::int16_t maximum[9] = {};
    std::int16_t minimum[9] = {};
  };

  struct AdtScanResult
  {
    //! False when the top-level walk hit a truncated chunk or trailing bytes.
    bool structurally_valid = false;

    bool mver_present = false;
    std::uint32_t mver = 0;

    bool has_mhdr = false;
    MHDR mhdr{};
    bool has_mcin = false;
    bool has_mtex = false;
    bool has_mmdx = false;
    bool has_mmid = false;
    bool has_mwmo = false;
    bool has_mwid = false;
    bool has_mddf = false;
    bool has_modf = false;
    bool has_mcsr = false;
    bool has_mh2o = false;
    bool has_mfbo = false;
    bool has_mtxf = false;

    //! Parsed when the MFBO chunk had exactly 36 bytes.
    bool mfbo_valid = false;
    MfboExtents mfbo{};

    //! Absolute offsets of every top-level MCNK chunk (header included).
    std::vector<std::size_t> mcnk_offsets;

    //! Top-level chunks outside the known set, fourcc -> count.
    std::map<std::uint32_t, unsigned int> unknown_chunks;
    //! MCNK subchunk anomalies found during validation.
    std::size_t mcnk_anomalies = 0;

    std::vector<AdtIssue> issues;
  };

  AdtScanResult scanAdt(char const* data, std::size_t size, AdtScanOptions const& options);
  inline AdtScanResult scanAdt(std::vector<std::uint8_t> const& data, AdtScanOptions const& options)
  {
    return scanAdt(reinterpret_cast<char const*>(data.data()), data.size(), options);
  }

  AdtLayout classifyAdtLayout(AdtScanResult const& scan);

  // ---- TEX0 ----------------------------------------------------------------

  struct ModernTextureLayer
  {
    std::uint32_t texture_id = 0;
    std::uint32_t file_data_id = 0;
    std::uint32_t flags = 0;
    std::uint32_t alpha_offset = 0;
    std::uint32_t effect_id = 0xFFFFFFFFu;
    std::uint32_t height_file_data_id = 0;
    bool has_height_id_table = false;
    bool disable_height_texture = false;
    bool has_mtxp = false;
    // MTXP parameters (mirrors texture_heightmapping_data without pulling in
    // the Qt-dependent TextureManager header).
    std::uint32_t mtxp_uv_scale = 0;
    float mtxp_height_scale = 0.0f;
    float mtxp_height_offset = 1.0f;
  };

  struct ModernTexChunk
  {
    std::vector<ModernTextureLayer> layers;
    std::vector<std::uint8_t> mcal;
  };

  struct ModernTextureParameters
  {
    std::uint32_t flags = 0;
    float height_scale = 0.0f;
    float height_offset = 1.0f;
  };

  struct ModernTex0Data
  {
    std::vector<std::uint32_t> diffuse_ids;
    std::vector<std::uint32_t> height_ids;
    std::vector<ModernTextureParameters> texture_parameters;
    bool has_height_ids_chunk = false;
    std::vector<ModernTexChunk> chunks;
  };

  struct Tex0ParseResult
  {
    //! False on any fatal problem; data must then not be used.
    bool ok = false;
    ModernTex0Data data;
    std::vector<AdtIssue> issues;
  };

  Tex0ParseResult parseTex0(char const* data, std::size_t size);
  inline Tex0ParseResult parseTex0(std::vector<std::uint8_t> const& data)
  {
    return parseTex0(reinterpret_cast<char const*>(data.data()), data.size());
  }

  //! Expand one layer's MCAL data to 4096 8-bit alpha values. Handles the
  //! compressed (RLE-ish) form, raw 4096-byte and legacy 4-bit 2048-byte
  //! forms. Returns false when the layer has no alpha or the data is short.
  bool decodeModernAlpha(ModernTexChunk const& tex_chunk,
                         ModernTextureLayer const& layer,
                         std::size_t layer_index,
                         std::vector<std::uint8_t>& output);

  // ---- OBJ0 -----------------------------------------------------------------

  struct Obj0ParseResult
  {
    bool ok = false;
    std::vector<ENTRY_MDDF> m2_placements;
    std::vector<ENTRY_MODF> wmo_placements;
    std::vector<AdtIssue> issues;
  };

  Obj0ParseResult parseObj0(char const* data, std::size_t size);
  inline Obj0ParseResult parseObj0(std::vector<std::uint8_t> const& data)
  {
    return parseObj0(reinterpret_cast<char const*>(data.data()), data.size());
  }

  // ---- helpers ---------------------------------------------------------------

  //! Render a chunk fourcc (canonical MSB-first value as compared against the
  //! little-endian magic read from disk) as printable name or hex.
  std::string adtChunkName(std::uint32_t fourcc);
  std::string adtLayoutName(AdtLayout layout);
  std::string severityName(AdtIssueSeverity severity);
}
