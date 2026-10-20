// This file is part of Noggit3, licensed under GNU General Public License (version 3).

//! \brief Version-aware reader for World Definition Table (WDT) files.
//!
//! The classic Noggit WDT loader read MVER, MPHD, MAIN and (optionally)
//! MWMO + MODF strictly sequentially. That works for the legacy (WotLK 3.3.5)
//! layout, but modern clients (Legion through Shadowlands 9.2.7 and later)
//! add chunks that Noggit historically never saw:
//!
//!   - MAID (8.1+, with MPHD flag 0x200): eight FileDataIDs per tile
//!     (root ADT, obj0, obj1, tex0, lod, map texture, map texture normals,
//!     minimap texture). The client loads ADTs through these ids instead of
//!     "World\\Maps\\<map>\\<map>_<x>_<y>.adt".
//!   - MAI2 (12.0.5+): additional per-tile FileDataIDs.
//!   - MANM (9.0.1+): map animation data.
//!   - MWMO is gone on modern clients for WMO-only maps: MODF carries the
//!     global WMO directly and its nameID field is a FileDataID (flag 0x8,
//!     "modf entry is file data id", Legion+), not an index into MWMO.
//!
//! On top of that, MPHD flag 0x2 (FLAG_SHADING in the legacy enum) means
//! "every ADT of this map has MCCV" on modern clients - it must not be
//! force-set there, and forcing it marked the WDT as changed, which made
//! Noggit rewrite (and thereby destroy) modern WDT files with the legacy
//! writer.
//!
//! This module parses a WDT byte buffer into a plain data structure and
//! collects validation issues instead of asserting, so callers can log
//! aggregated diagnostics. It deliberately contains no Qt and no logging
//! dependencies so it can be unit tested standalone.

#pragma once

#include <noggit/MapHeaders.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Noggit::Formats::WDT
{
  enum class IssueSeverity
  {
    info,
    warning,
    error
  };

  struct ParseIssue
  {
    IssueSeverity severity = IssueSeverity::info;
    std::string message;
  };

  //! MAID payload of a single tile: the eight ADT/texture FileDataIDs the
  //! modern client uses to locate the files of tile (x, y).
  struct MaidEntry
  {
    std::uint32_t root_adt = 0;
    std::uint32_t obj0_adt = 0;
    std::uint32_t obj1_adt = 0;
    std::uint32_t tex0_adt = 0;
    std::uint32_t lod_adt = 0;
    std::uint32_t map_texture = 0;
    std::uint32_t map_texture_normals = 0;
    std::uint32_t minimap_texture = 0;
  };

  struct ParsedWdt
  {
    //! True when MVER, MPHD and MAIN were present, correctly sized and no
    //! error-severity issue was recorded. Callers may still use partially
    //! parsed data, but should surface the issues.
    bool structurally_valid = false;

    bool mver_present = false;
    std::uint32_t version = 0;

    bool mphd_present = false;
    MPHD mphd{};
    //! Byte offset of the MPHD payload inside the source buffer (for the
    //! in-place patch writer). npos when absent.
    std::size_t mphd_payload_offset = static_cast<std::size_t>(-1);

    bool main_present = false;
    //! MAIN chunk data, indexed as [y * 64 + x] like the file itself.
    std::array<std::uint32_t, 4096> main_flags{};
    std::array<std::uint32_t, 4096> main_async_ids{};
    std::size_t main_payload_offset = static_cast<std::size_t>(-1);

    //! MAID (8.1+). Present when the chunk was found and correctly sized.
    bool maid_present = false;
    std::vector<MaidEntry> maid;

    //! Global WMO. Legacy layout: MWMO (zero terminated file name) + MODF
    //! (placement, nameID indexes MWMO). Modern layout: MODF only, with the
    //! FileDataID flag (0x8) set and nameID being the FileDataID of the WMO.
    bool mwmo_present = false;
    std::string mwmo_name;
    bool modf_present = false;
    ENTRY_MODF modf{};
    bool modf_name_is_fdid = false;

    //! Chunks that are known but unused by Noggit (MANM, MAI2, light/fog/...
    //! FileDataID chunks), fourcc -> occurrence count.
    std::map<std::uint32_t, unsigned int> known_skipped;
    //! Chunks Noggit does not recognize at all, fourcc -> occurrence count.
    std::map<std::uint32_t, unsigned int> unknown_chunks;

    std::vector<ParseIssue> issues;
  };

  //! Parse a complete WDT file image.
  //! \param data        raw file bytes
  //! \param modern_client true for post-WotLK clients (BfA/Cata/... 4.x+);
  //!        controls which MPHD flag semantics and global WMO layouts are
  //!        expected during validation.
  ParsedWdt parseWdt(std::vector<std::uint8_t> const& data, bool modern_client);

  struct PatchResult
  {
    bool applied = false;
    std::vector<ParseIssue> issues;
  };

  //! Patch an already parsed (and still unmodified) WDT image in place:
  //! replaces the MPHD flags word and the 4096 MAIN entry flags while every
  //! other chunk (MAID, MANM, MWMO/MODF, unknown chunks, ...) is preserved
  //! byte for byte. Use this to save modern WDTs instead of rebuilding them
  //! with the legacy writer.
  PatchResult applyWdtEdits(std::vector<std::uint8_t>& data,
                            ParsedWdt const& parsed,
                            std::uint32_t mphd_flags,
                            std::vector<std::uint32_t> const& main_flags);

  //! Render a chunk fourcc as its printable name or as hex.
  std::string chunkName(std::uint32_t fourcc);
  std::string severityName(IssueSeverity severity);
}
