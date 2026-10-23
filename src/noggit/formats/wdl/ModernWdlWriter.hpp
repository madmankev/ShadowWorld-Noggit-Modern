#pragma once

//! \brief Native writer for the modern (Legion+) WDL format.
//!
//! Two gaps in the WDL save path made a modern map's low-detail file wrong:
//!
//!  * A map whose WDL was never loaded from disk (new maps from the wizard,
//!    maps installed without a WDL) was saved with the **legacy** writer:
//!    MWMO/MWID/MODF object chunks a Legion+ client does not read, horizon
//!    WMOs referenced by file name instead of FileDataID, and no MLMD/MLMX.
//!  * When a source WDL existed, heights were patched in place - correct and
//!    preserving - but tiles the source had no MARE slot for could not be
//!    persisted at all ("their horizon heights are not persisted").
//!
//! This module closes both: `writeModernWdl` builds a complete modern WDL
//! (MVER 18, MAOF, per-tile MARE + MAHO, MLMD + MLMX) from the horizon data
//! map_horizon already holds, and `rewriteModernWdl` rebuilds an existing
//! modern WDL with updated/added tile payloads while every other chunk -
//! MLMD/MLMX placements, MSSN/MSSC/MSSO sky scenes, MSLD/MSLI liquids,
//! MAOE/MAOC oceans, anything unknown or future - is preserved byte for byte.
//!
//! The chunk set and record layouts match `Conversion::buildModernWdl` (the
//! harness-proven conversion writer) and the tree's own WDL loader
//! (`map_horizon`): MAOF entries are absolute offsets of the MARE chunk tag,
//! MARE payloads are 17x17 followed by 16x16 int16 heights (1090 bytes),
//! MAHO payloads are 16 int16 hole masks (32 bytes), MLMD records are 40
//! bytes (ENTRY_MODF without the extents block, nameID = FileDataID, the
//! 0x8 flag cleared) and MLMX records are 28 bytes (min, max, radius).
//!
//! Qt-free and dependency-free so both functions can be unit tested.

#include <noggit/MapHeaders.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Noggit::Formats::WDL
{
  //! One horizon WMO placement for MLMD/MLMX.
  struct ModernWdlObject
  {
    //! Resolved CASC FileDataID of the WMO. Entries with fdid 0 are skipped
    //! by the writer with a note - callers resolve names through the client
    //! listfile and drop what it cannot resolve, rather than inventing ids.
    std::uint32_t fdid = 0;
    //! Placement in the legacy ENTRY_MODF shape the editor works with:
    //! nameID is ignored (fdid above wins), extents[0] = upper and
    //! extents[1] = lower corner feed MLMX, scale uses the 1024 = 1.0
    //! semantics.
    ENTRY_MODF placement{};
  };

  //! Low-detail payload of one tile: exactly what a MARE chunk (height_17
  //! then height_16) and a MAHO chunk (holes) carry.
  struct ModernWdlTile
  {
    std::int16_t height_17[17][17]{};
    std::int16_t height_16[16][16]{};
    std::int16_t holes[16]{};
  };

  struct ModernWdlInput
  {
    //! Present tiles, indexed [y * 64 + x] like the MAOF table. Sparse on
    //! purpose: real maps hold a few hundred tiles, and a dense 4096-entry
    //! array would make every input a multi-megabyte object. Membership in
    //! the map is what makes a tile present; indices >= 4096 are skipped
    //! with a note.
    std::map<std::size_t, ModernWdlTile> tiles;
    //! Horizon WMO placements, emitted as MLMD + MLMX when non-empty.
    std::vector<ModernWdlObject> objects;
  };

  struct ModernWdlResult
  {
    bool ok = false;
    std::vector<std::uint8_t> bytes;
    //! What was written/preserved/skipped, for the caller's log.
    std::vector<std::string> notes;
  };

  //! Build a complete modern WDL from scratch. Layout: MVER(18), MAOF
  //! (4096 absolute u32 offsets of MARE tags, 0 = no tile), the present
  //! tiles' MARE + MAHO chunks in tile order, then MLMD + MLMX when objects
  //! were given. Sky scenes and other optional chunks are client-authored
  //! data Noggit does not have for a map that never had a WDL, so they are
  //! left out rather than fabricated.
  ModernWdlResult writeModernWdl(ModernWdlInput const& input);

  struct WdlRewriteInput
  {
    //! The modern WDL image to rebuild (as loaded from client or project).
    std::vector<std::uint8_t> original;
    //! Replacement tile payloads, indexed [y * 64 + x]. Tiles not listed
    //! keep their original MARE/MAHO bytes; listed tiles whose slot the
    //! original lacked get freshly emitted chunks - that is the fix for
    //! source WDLs with no MARE slot for tiles created in the editor.
    std::map<std::size_t, ModernWdlTile> tiles;
  };

  //! Rebuild a modern WDL with updated tile payloads. MAOF is recomputed
  //! against the emitted layout; every chunk that is not MVER, MAOF or a
  //! MAOF-referenced MARE/MAHO pair is copied through byte for byte in its
  //! original order. Fails (ok == false, explanatory note) when the original
  //! does not walk as a chunk file - a file it cannot account for completely
  //! is a file it must not rebuild.
  ModernWdlResult rewriteModernWdl(WdlRewriteInput const& input);
}
