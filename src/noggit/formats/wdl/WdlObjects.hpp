// This file is part of Noggit3, licensed under GNU General Public License (version 3).

//! \brief Parser for the modern (Legion+) WDL object chunks MLMD / MLMX.
//!
//! Legacy WDL files (< Legion) store horizon WMOs as MWMO (file name list)
//! + MWID + MODF (64 byte placements whose nameID indexes MWMO). Modern WDL
//! files dropped those chunks: MLMD holds the WMO placements as 40 byte
//! records whose nameID is a CASC FileDataID (same record layout as the
//! MLMD chunk of _obj1.adt), and MLMX holds the matching bounds extents
//! (28 bytes per record, same as _obj1.adt).
//!
//! This module converts MLMD + MLMX payloads into the legacy shaped data
//! Noggit's map_horizon works with (a de-duplicated file name list plus
//! ENTRY_MODF instances whose nameID indexes that list), resolving the
//! FileDataIDs through a caller supplied callback (normally the CASC
//! listfile). It contains no Qt and no logging so it can be unit tested.

#pragma once

#include <noggit/MapHeaders.h>
#include <noggit/formats/wdt/WdtReader.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Noggit::Formats::WDL
{
  using IssueSeverity = Noggit::Formats::WDT::IssueSeverity;
  using ParseIssue = Noggit::Formats::WDT::ParseIssue;

  struct ModernObjectParseResult
  {
    //! De-duplicated resolved file names. ENTRY_MODF::nameID below indexes
    //! into this list (caller has to rebase the ids when appending to its
    //! own file name list).
    std::vector<std::string> filenames;
    //! Placements in legacy ENTRY_MODF shape: pos/rot/flags/doodadSet/
    //! nameSet copied from MLMD, extents filled from MLMX (upper extents
    //! first, like MODF), scale normalized to 1024 = 1.0 semantics.
    std::vector<ENTRY_MODF> instances;
    std::vector<ParseIssue> issues;
    //! MLMD records seen, resolved and skipped (unresolved FileDataIDs).
    unsigned int record_count = 0;
    unsigned int skipped_count = 0;
  };

  //! Parse MLMD (+ optional MLMX) payloads.
  //! \param mlmd_payload raw MLMD chunk payload (40 bytes per record)
  //! \param mlmx_payload raw MLMX chunk payload (28 bytes per record, may be
  //!        empty - extents then default to zero)
  //! \param fdid_to_path resolves a FileDataID to a file path; returns an
  //!        empty string when the id is unknown to the listfile
  ModernObjectParseResult parseModernWdlObjects(
    std::vector<std::uint8_t> const& mlmd_payload,
    std::vector<std::uint8_t> const& mlmx_payload,
    std::function<std::string(std::uint32_t)> const& fdid_to_path);
}
