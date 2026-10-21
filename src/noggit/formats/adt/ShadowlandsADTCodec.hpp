// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Shadowlands split-ADT codec boundary.
// The design follows the verified split ADT/semantic round-trip model documented
// by skarndev/wowlib, while remaining C++20/MSVC compatible with Noggit.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Noggit::Formats::ADT
{
  enum class ShadowlandsADTPart : std::uint8_t
  {
    Root = 0,
    Tex0,
    Obj0,
    Obj1,
    Lod,
    Count
  };

  //! FileDataIDs the editor already knows, keyed by normalized asset path.
  //!
  //! The legacy ADT serialization only carries path strings (MTEX/MMDX/MWMO), so
  //! converting back to the modern split layout has to recover a FileDataID for
  //! every one of them. The project listfile is not complete on retail installs:
  //! it has no entry for assets whose id Noggit had to guess, and none for files
  //! missing from a partial CASC. The in-memory FileKeys do carry the real id, so
  //! MapTile::save hands them over and the conversion no longer depends on
  //! listfile coverage.
  using ModernAdtFdidHints = std::unordered_map<std::string, std::uint32_t>;

  //! What a modern tile save could not carry over.
  //!
  //! Nothing in here aborts the save. Losing one texture layer or one placement
  //! is visible and recoverable; losing the whole tile - heights, textures,
  //! objects and water together - because a single path could not be resolved is
  //! what used to happen, and it looked like "saving does nothing".
  struct ModernAdtSaveReport
  {
    //! Texture paths with no FileDataID: the layer is written with id 0 and
    //! renders untextured.
    std::size_t unresolved_textures = 0;
    //! Placement paths with no FileDataID whose original id could be recovered
    //! from the tile being rewritten (an untouched object, so nothing is lost).
    std::size_t recovered_placements = 0;
    //! Placements that could not be resolved at all and were dropped.
    std::size_t skipped_placements = 0;

    //! True when the parts were synthesized from the edited tile because the
    //! original split tile could not be merged into. The result is still a
    //! modern split ADT, but chunks that only exist in the retail file and have
    //! no legacy equivalent (MTXP height textures, MHID) are absent.
    bool synthesized = false;
    //! Assets written with a FileDataID the converter had to invent because
    //! neither the tile nor the project listfile knew one. The client has no
    //! such id, so those assets stay invisible until they are registered.
    std::size_t custom_fdids = 0;

    std::string first_unresolved_texture;
    std::string first_recovered_placement;
    std::string first_skipped_placement;
    std::string first_custom_fdid;

    [[nodiscard]] bool empty() const noexcept
    {
      return !unresolved_textures && !recovered_placements && !skipped_placements
             && !synthesized && !custom_fdids;
    }
  };

  struct ShadowlandsADTSource
  {
    std::string root_path;
    std::array<std::string, static_cast<std::size_t>(ShadowlandsADTPart::Count)> paths;
  };

  struct ShadowlandsADTRawPart
  {
    bool present = false;
    std::vector<std::uint8_t> bytes;
  };

  // Raw backing store for a single logical Shadowlands ADT tile.  Keeping all
  // split parts here is deliberate: until a field has a native writer, its
  // original bytes can be preserved instead of being silently dropped by the
  // legacy WotLK serializer.
  struct ShadowlandsADTBackingStore
  {
    ShadowlandsADTSource source;
    std::array<ShadowlandsADTRawPart, static_cast<std::size_t>(ShadowlandsADTPart::Count)> parts;

    [[nodiscard]] bool has(ShadowlandsADTPart part) const noexcept;
    [[nodiscard]] ShadowlandsADTRawPart const& part(ShadowlandsADTPart part) const noexcept;
    [[nodiscard]] ShadowlandsADTRawPart& part(ShadowlandsADTPart part) noexcept;
  };

  class ShadowlandsADTCodec
  {
  public:
    [[nodiscard]] static ShadowlandsADTSource makeSource(std::string const& root_path);

    // Loads the physical split files through Noggit's ClientData/CASC gateway.
    // ROOT, TEX0 and OBJ0 are required for the current Shadowlands terrain
    // pipeline; OBJ1 and LOD are retained when present.
    [[nodiscard]] static ShadowlandsADTBackingStore loadBackingStore(std::string const& root_path);

    // Converts Noggit's canonical legacy serialization back into the modern
    // split layout while retaining chunks that the editor does not own.
    // Kept as the fallback path; the native writer (ModernAdtWriter) is the
    // primary ROOT producer. Both paths now refresh the ROOT bookkeeping
    // (MCIN offsets/sizes, MHDR mcin/mh2o/mfbo) and validate every part
    // before anything is written to disk.
    static void saveFromLegacy(ShadowlandsADTBackingStore const& backing,
                               std::vector<std::uint8_t> const& legacy_adt,
                               ModernAdtFdidHints const& fdid_hints = {},
                               ModernAdtSaveReport* report = nullptr);

    // Saves a ROOT part that ModernAdtWriter assembled natively, converting
    // TEX0/OBJ0/OBJ1 from the legacy serialization as before. Throws
    // ModernAdtValidationError when any assembled part fails validation;
    // nothing is written in that case.
    static void saveNativeRoot(ShadowlandsADTBackingStore const& backing,
                               std::vector<std::uint8_t> root,
                               std::vector<std::uint8_t> const& legacy_adt,
                               ModernAdtFdidHints const& fdid_hints = {},
                               ModernAdtSaveReport* report = nullptr);
  };
}
