// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/formats/wdl/WdlObjects.hpp>

#include <cstring>
#include <map>
#include <sstream>

namespace Noggit::Formats::WDL
{
  namespace
  {
    //! MLMD record - identical to the MLMD chunk of _obj1.adt (Legion+):
    //! an ENTRY_MODF without the 24 byte extents block (those live in MLMX).
    struct MlmdRecord
    {
      std::uint32_t nameID;     // FileDataID of the WMO
      std::uint32_t uniqueID;
      float pos[3];
      float rot[3];
      std::uint16_t flags;
      std::uint16_t doodadSet;
      std::uint16_t nameSet;
      std::uint16_t scale;
    };
    static_assert(sizeof(MlmdRecord) == 40, "MLMD records are 40 bytes");

    //! MLMX record - bounds of the matching MLMD entry (as in _obj1.adt).
    struct MlmxRecord
    {
      float min[3];
      float max[3];
      float radius;
    };
    static_assert(sizeof(MlmxRecord) == 28, "MLMX records are 28 bytes");

    //! MODF/MLMD flags bit meaning "the scale field is valid" (Legion+).
    //! When unset, retail treats the placement scale as 1.0 regardless of
    //! the stored value.
    constexpr std::uint16_t MLMD_FLAG_HAS_SCALE = 0x4;

    std::uint32_t readU32(std::uint8_t const* p)
    {
      std::uint32_t value = 0;
      std::memcpy(&value, p, sizeof(value));
      return value;
    }

    float readFloat(std::uint8_t const* p)
    {
      float value = 0.0f;
      std::memcpy(&value, p, sizeof(value));
      return value;
    }

    std::uint16_t readU16(std::uint8_t const* p)
    {
      std::uint16_t value = 0;
      std::memcpy(&value, p, sizeof(value));
      return value;
    }
  }

  ModernObjectParseResult parseModernWdlObjects(
    std::vector<std::uint8_t> const& mlmd_payload,
    std::vector<std::uint8_t> const& mlmx_payload,
    std::function<std::string(std::uint32_t)> const& fdid_to_path)
  {
    ModernObjectParseResult result;

    auto add_issue = [&result](IssueSeverity severity, std::string message)
    {
      result.issues.push_back(ParseIssue{severity, std::move(message)});
    };

    if (mlmd_payload.empty())
      return result;

    if (mlmd_payload.size() % sizeof(MlmdRecord) != 0)
    {
      std::ostringstream message;
      message << "MLMD payload size " << mlmd_payload.size()
              << " is not a multiple of " << sizeof(MlmdRecord)
              << "; horizon WMO placements cannot be parsed";
      add_issue(IssueSeverity::error, message.str());
      return result;
    }

    std::size_t const record_count = mlmd_payload.size() / sizeof(MlmdRecord);
    result.record_count = static_cast<unsigned int>(record_count);

    bool mlmx_usable = false;
    if (!mlmx_payload.empty())
    {
      if (mlmx_payload.size() % sizeof(MlmxRecord) != 0)
      {
        std::ostringstream message;
        message << "MLMX payload size " << mlmx_payload.size()
                << " is not a multiple of " << sizeof(MlmxRecord) << "; bounds ignored";
        add_issue(IssueSeverity::warning, message.str());
      }
      else if (mlmx_payload.size() / sizeof(MlmxRecord) != record_count)
      {
        std::ostringstream message;
        message << "MLMX holds " << mlmx_payload.size() / sizeof(MlmxRecord)
                << " bounds records but MLMD holds " << record_count
                << " placements; bounds ignored";
        add_issue(IssueSeverity::warning, message.str());
      }
      else
      {
        mlmx_usable = true;
      }
    }
    else if (record_count > 0)
    {
      add_issue(IssueSeverity::info,
                "WDL has MLMD placements but no MLMX bounds; extents default to zero");
    }

    std::map<std::string, std::uint32_t> filename_to_index;

    for (std::size_t i = 0; i < record_count; ++i)
    {
      std::uint8_t const* record = mlmd_payload.data() + i * sizeof(MlmdRecord);

      MlmdRecord mlmd{};
      mlmd.nameID = readU32(record + 0);
      mlmd.uniqueID = readU32(record + 4);
      for (int c = 0; c < 3; ++c)
      {
        mlmd.pos[c] = readFloat(record + 8 + c * 4);
        mlmd.rot[c] = readFloat(record + 20 + c * 4);
      }
      mlmd.flags = readU16(record + 32);
      mlmd.doodadSet = readU16(record + 34);
      mlmd.nameSet = readU16(record + 36);
      mlmd.scale = readU16(record + 38);

      std::string path;
      if (fdid_to_path)
        path = fdid_to_path(mlmd.nameID);

      if (path.empty())
      {
        std::ostringstream message;
        message << "horizon WMO placement " << i << " references FileDataID "
                << mlmd.nameID << " which the listfile cannot resolve; placement skipped";
        add_issue(IssueSeverity::warning, message.str());
        ++result.skipped_count;
        continue;
      }

      std::uint32_t name_index = 0;
      auto const known = filename_to_index.find(path);
      if (known != filename_to_index.end())
      {
        name_index = known->second;
      }
      else
      {
        name_index = static_cast<std::uint32_t>(result.filenames.size());
        filename_to_index.emplace(path, name_index);
        result.filenames.push_back(path);
      }

      ENTRY_MODF entry{};
      entry.nameID = name_index;
      entry.uniqueID = mlmd.uniqueID;
      for (int c = 0; c < 3; ++c)
      {
        entry.pos[c] = mlmd.pos[c];
        entry.rot[c] = mlmd.rot[c];
      }

      if (mlmx_usable)
      {
        std::uint8_t const* bounds = mlmx_payload.data() + i * sizeof(MlmxRecord);
        MlmxRecord mlmx{};
        for (int c = 0; c < 3; ++c)
        {
          mlmx.min[c] = readFloat(bounds + c * 4);
          mlmx.max[c] = readFloat(bounds + 12 + c * 4);
        }
        mlmx.radius = readFloat(bounds + 24);
        // MODF stores upper extents first, lower extents second.
        entry.extents[0] = glm::vec3{mlmx.max[0], mlmx.max[1], mlmx.max[2]};
        entry.extents[1] = glm::vec3{mlmx.min[0], mlmx.min[1], mlmx.min[2]};
      }

      entry.flags = mlmd.flags;
      entry.doodadSet = mlmd.doodadSet;
      entry.nameSet = mlmd.nameSet;

      // Legacy ENTRY_MODF consumers (e.g. map_horizon::wmoHasLowRes) always
      // divide scale by 1024. Modern MLMD only carries a meaningful scale
      // when flag 0x4 is set; otherwise the placement scale is 1.0.
      if ((mlmd.flags & MLMD_FLAG_HAS_SCALE) == 0 || mlmd.scale == 0)
        entry.scale = 1024;
      else
        entry.scale = mlmd.scale;

      result.instances.push_back(entry);
    }

    return result;
  }
}
