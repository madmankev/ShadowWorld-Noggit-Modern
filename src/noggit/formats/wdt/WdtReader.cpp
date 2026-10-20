// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/formats/wdt/WdtReader.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace Noggit::Formats::WDT
{
  namespace
  {
    constexpr std::uint32_t makeFourcc(char a, char b, char c, char d)
    {
      return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8)
           | static_cast<std::uint32_t>(static_cast<unsigned char>(d));
    }

    constexpr std::uint32_t FOURCC_MVER = makeFourcc('M', 'V', 'E', 'R');
    constexpr std::uint32_t FOURCC_MPHD = makeFourcc('M', 'P', 'H', 'D');
    constexpr std::uint32_t FOURCC_MAIN = makeFourcc('M', 'A', 'I', 'N');
    constexpr std::uint32_t FOURCC_MAID = makeFourcc('M', 'A', 'I', 'D');
    constexpr std::uint32_t FOURCC_MWMO = makeFourcc('M', 'W', 'M', 'O');
    constexpr std::uint32_t FOURCC_MODF = makeFourcc('M', 'O', 'D', 'F');

    // Chunks that exist in retail WDTs but that Noggit does not need.
    constexpr std::uint32_t FOURCC_MAI2 = makeFourcc('M', 'A', 'I', '2');
    constexpr std::uint32_t FOURCC_MANM = makeFourcc('M', 'A', 'N', 'M');
    constexpr std::uint32_t FOURCC_MLID = makeFourcc('M', 'L', 'I', 'D');
    constexpr std::uint32_t FOURCC__OCC = makeFourcc('_', 'o', 'c', 'c');
    constexpr std::uint32_t FOURCC__LGT = makeFourcc('_', 'l', 'g', 't');
    constexpr std::uint32_t FOURCC__FOGS = makeFourcc('_', 'f', 'o', 'g');
    constexpr std::uint32_t FOURCC__MPV = makeFourcc('_', 'm', 'p', 'v');
    constexpr std::uint32_t FOURCC__TEX = makeFourcc('_', 't', 'e', 'x');
    constexpr std::uint32_t FOURCC__WDL = makeFourcc('_', 'w', 'd', 'l');
    constexpr std::uint32_t FOURCC__PD4 = makeFourcc('_', 'p', 'd', '4');

    //! MODF flags bit meaning "nameID is a FileDataID" (Legion and later).
    constexpr std::uint16_t MODF_FLAG_NAME_IS_FDid = 0x8;

    std::uint32_t readU32(std::uint8_t const* p)
    {
      std::uint32_t value = 0;
      std::memcpy(&value, p, sizeof(value));
      return value;
    }

    void writeU32(std::uint8_t* p, std::uint32_t value)
    {
      std::memcpy(p, &value, sizeof(value));
    }

    bool isKnownSkippedChunk(std::uint32_t fourcc)
    {
      switch (fourcc)
      {
        case FOURCC_MAI2:
        case FOURCC_MANM:
        case FOURCC_MLID:
        case FOURCC__OCC:
        case FOURCC__LGT:
        case FOURCC__FOGS:
        case FOURCC__MPV:
        case FOURCC__TEX:
        case FOURCC__WDL:
        case FOURCC__PD4:
          return true;
        default:
          return false;
      }
    }
  }

  std::string chunkName(std::uint32_t fourcc)
  {
    char name[5] = {};
    bool printable = true;

    for (int index = 0; index < 4; ++index)
    {
      auto const character = static_cast<char>((fourcc >> (24 - 8 * index)) & 0xFFu);
      if (std::isprint(static_cast<unsigned char>(character)) == 0)
        printable = false;
      name[index] = character;
    }

    if (!printable)
    {
      std::ostringstream hex;
      hex << "0x" << std::hex << std::uppercase << fourcc;
      return hex.str();
    }

    return std::string(name, 4);
  }

  std::string severityName(IssueSeverity severity)
  {
    switch (severity)
    {
      case IssueSeverity::info: return "info";
      case IssueSeverity::warning: return "warning";
      default: return "error";
    }
  }

  ParsedWdt parseWdt(std::vector<std::uint8_t> const& data, bool modern_client)
  {
    static_assert(sizeof(MPHD) == 32, "MPHD must stay 32 bytes (flags + 7 u32 fields)");
    static_assert(sizeof(ENTRY_MODF) == 64, "ENTRY_MODF must stay 64 bytes");

    ParsedWdt parsed;

    auto add_issue = [&parsed](IssueSeverity severity, std::string message)
    {
      parsed.issues.push_back(ParseIssue{severity, std::move(message)});
    };

    if (data.empty())
    {
      add_issue(IssueSeverity::error, "WDT file is empty");
      return parsed;
    }

    std::size_t pos = 0;
    while (pos + 8 <= data.size())
    {
      std::uint32_t const fourcc = readU32(data.data() + pos);
      std::uint32_t const size = readU32(data.data() + pos + 4);
      std::size_t const payload_offset = pos + 8;

      if (size > data.size() - payload_offset)
      {
        std::ostringstream message;
        message << "chunk " << chunkName(fourcc) << " at offset " << pos
                << " claims " << size << " bytes but only "
                << (data.size() - payload_offset) << " remain - file truncated or corrupt";
        add_issue(IssueSeverity::error, message.str());
        break;
      }

      std::uint8_t const* payload = data.data() + payload_offset;

      switch (fourcc)
      {
        case FOURCC_MVER:
        {
          parsed.mver_present = true;
          if (size >= 4)
            parsed.version = readU32(payload);
          if (size != 4)
          {
            std::ostringstream message;
            message << "MVER chunk has unexpected size " << size << " (expected 4)";
            add_issue(IssueSeverity::warning, message.str());
          }
          if (parsed.version != 18)
          {
            std::ostringstream message;
            message << "unsupported WDT version " << parsed.version << " (expected 18)";
            add_issue(IssueSeverity::error, message.str());
          }
          break;
        }

        case FOURCC_MPHD:
        {
          parsed.mphd_present = true;
          parsed.mphd_payload_offset = payload_offset;
          if (size < 4)
          {
            std::ostringstream message;
            message << "MPHD chunk too small (" << size << " bytes), cannot read flags";
            add_issue(IssueSeverity::error, message.str());
            break;
          }
          std::memcpy(&parsed.mphd, payload, std::min<std::size_t>(size, sizeof(MPHD)));
          if (size < sizeof(MPHD))
          {
            std::ostringstream message;
            message << "MPHD chunk is " << size << " bytes (expected " << sizeof(MPHD)
                    << "); modern FileDataID fields (lgt/occ/fogs/mpv/tex/wdl) are missing";
            add_issue(IssueSeverity::warning, message.str());
          }
          break;
        }

        case FOURCC_MAIN:
        {
          parsed.main_present = true;
          parsed.main_payload_offset = payload_offset;
          if (size != 4096 * 8)
          {
            std::ostringstream message;
            message << "MAIN chunk has unexpected size " << size << " (expected "
                    << 4096 * 8 << " = 4096 tile entries of 8 bytes)";
            add_issue(IssueSeverity::error, message.str());
            break;
          }
          for (std::size_t i = 0; i < 4096; ++i)
          {
            parsed.main_flags[i] = readU32(payload + i * 8);
            parsed.main_async_ids[i] = readU32(payload + i * 8 + 4);
          }
          break;
        }

        case FOURCC_MAID:
        {
          if (size != 4096 * 32)
          {
            std::ostringstream message;
            message << "MAID chunk has unexpected size " << size << " (expected "
                    << 4096 * 32 << " = 4096 tile entries of 8 FileDataIDs)";
            add_issue(IssueSeverity::error, message.str());
            break;
          }
          parsed.maid_present = true;
          parsed.maid.resize(4096);
          for (std::size_t i = 0; i < 4096; ++i)
          {
            MaidEntry& entry = parsed.maid[i];
            entry.root_adt = readU32(payload + i * 32 + 0);
            entry.obj0_adt = readU32(payload + i * 32 + 4);
            entry.obj1_adt = readU32(payload + i * 32 + 8);
            entry.tex0_adt = readU32(payload + i * 32 + 12);
            entry.lod_adt = readU32(payload + i * 32 + 16);
            entry.map_texture = readU32(payload + i * 32 + 20);
            entry.map_texture_normals = readU32(payload + i * 32 + 24);
            entry.minimap_texture = readU32(payload + i * 32 + 28);
          }
          break;
        }

        case FOURCC_MWMO:
        {
          parsed.mwmo_present = true;
          if (size == 0)
          {
            add_issue(IssueSeverity::warning, "MWMO chunk is empty");
            break;
          }
          if (size > 0x10000)
          {
            std::ostringstream message;
            message << "MWMO chunk is suspiciously large (" << size << " bytes)";
            add_issue(IssueSeverity::warning, message.str());
          }
          // Zero terminated file name(s); Noggit only ever uses the first.
          auto const* begin = reinterpret_cast<char const*>(payload);
          auto const* nul = static_cast<char const*>(
            std::memchr(payload, '\0', size));
          std::size_t const length = nul ? static_cast<std::size_t>(nul - begin) : size;
          parsed.mwmo_name.assign(begin, length);
          if (length + 1 < size)
          {
            std::ostringstream message;
            message << "MWMO chunk contains more than one file name (" << size
                    << " bytes); only the first is used";
            add_issue(IssueSeverity::info, message.str());
          }
          break;
        }

        case FOURCC_MODF:
        {
          if (size < sizeof(ENTRY_MODF))
          {
            std::ostringstream message;
            message << "MODF chunk too small (" << size << " bytes, expected a multiple of "
                    << sizeof(ENTRY_MODF) << ")";
            add_issue(IssueSeverity::error, message.str());
            break;
          }
          if (size % sizeof(ENTRY_MODF) != 0)
          {
            std::ostringstream message;
            message << "MODF chunk size " << size << " is not a multiple of "
                    << sizeof(ENTRY_MODF) << "; trailing bytes ignored";
            add_issue(IssueSeverity::warning, message.str());
          }
          parsed.modf_present = true;
          std::memcpy(&parsed.modf, payload, sizeof(ENTRY_MODF));
          parsed.modf_name_is_fdid = (parsed.modf.flags & MODF_FLAG_NAME_IS_FDid) != 0;
          if (size > sizeof(ENTRY_MODF))
          {
            std::ostringstream message;
            message << "MODF chunk holds " << (size / sizeof(ENTRY_MODF))
                    << " entries; only the first (the global WMO) is used";
            add_issue(IssueSeverity::info, message.str());
          }
          break;
        }

        default:
        {
          if (isKnownSkippedChunk(fourcc))
            ++parsed.known_skipped[fourcc];
          else
            ++parsed.unknown_chunks[fourcc];
          break;
        }
      }

      pos = payload_offset + size;
    }

    if (pos < data.size())
    {
      // The loop consumes every complete chunk header, so anything left
      // over is a partial header: the file ends in the middle of a chunk.
      std::ostringstream message;
      message << (data.size() - pos)
              << " trailing byte(s) at end of file: incomplete chunk header "
                 "(file truncated or corrupt)";
      add_issue(IssueSeverity::error, message.str());
    }

    // ---- structural validation -------------------------------------------

    if (!parsed.mver_present)
      add_issue(IssueSeverity::error, "WDT has no MVER chunk");

    if (!parsed.mphd_present)
      add_issue(IssueSeverity::error, "WDT has no MPHD chunk");

    if (!parsed.main_present)
      add_issue(IssueSeverity::error, "WDT has no MAIN chunk");

    // ---- semantic validation ----------------------------------------------

    bool const wants_global_wmo = (parsed.mphd.flags & FLAG_GLOBAL_OBJECT) != 0;
    bool const wants_maid = (parsed.mphd.flags & FLAG_HAS_MAID) != 0;

    if (modern_client)
    {
      if (wants_maid && !parsed.maid_present)
        add_issue(IssueSeverity::warning,
                  "MPHD flag 0x200 (client loads ADTs by FileDataID) is set but the WDT has "
                  "no MAID chunk; tile files will be looked up by name instead");

      if (wants_global_wmo && parsed.modf_present && !parsed.mwmo_present
          && !parsed.modf_name_is_fdid)
        add_issue(IssueSeverity::warning,
                  "modern WDT declares a global WMO via MODF but the MODF flags do not mark "
                  "nameID as a FileDataID (0x8) and no MWMO chunk exists; the global WMO name "
                  "cannot be resolved");

      if (wants_global_wmo && parsed.mwmo_present)
        add_issue(IssueSeverity::info,
                  "modern WDT still contains an MWMO chunk; its file name is used for the "
                  "global WMO");
    }
    else
    {
      if (parsed.maid_present)
        add_issue(IssueSeverity::info,
                  "legacy client but WDT contains MAID (FileDataID) data; it is ignored");

      if (parsed.modf_present && parsed.modf_name_is_fdid)
        add_issue(IssueSeverity::info,
                  "MODF carries the modern FileDataID flag (0x8) but a legacy client is "
                  "active; the MWMO file name is used when present");
    }

    if (wants_global_wmo && !parsed.mwmo_present && !parsed.modf_present)
      add_issue(IssueSeverity::warning,
                "MPHD flag 0x1 (global WMO) is set but neither MWMO nor MODF was found");

    if (!wants_global_wmo && (parsed.mwmo_present || parsed.modf_present))
      add_issue(IssueSeverity::info,
                "WDT contains global WMO chunks (MWMO/MODF) but MPHD flag 0x1 is not set; "
                "they are ignored");

    // MAID / MAIN consistency: tiles flagged as existing should have a root
    // ADT FileDataID when the client resolves files through MAID.
    if (parsed.maid_present)
    {
      unsigned int missing_root_fdids = 0;
      for (std::size_t i = 0; i < 4096; ++i)
      {
        if ((parsed.main_flags[i] & 1) && parsed.maid[i].root_adt == 0)
          ++missing_root_fdids;
      }
      if (missing_root_fdids > 0)
      {
        std::ostringstream message;
        message << missing_root_fdids
                << " tile(s) flagged as existing in MAIN have no root ADT FileDataID in MAID"
                << (modern_client ? " (newly created tiles need MAID registration to be "
                                  "visible to the modern client)" : "");
        add_issue(IssueSeverity::info, message.str());
      }
    }

    bool const has_error = std::any_of(parsed.issues.begin(), parsed.issues.end(),
      [](ParseIssue const& issue) { return issue.severity == IssueSeverity::error; });

    parsed.structurally_valid =
      parsed.mver_present && parsed.version == 18
      && parsed.mphd_present && parsed.main_present
      && parsed.main_payload_offset != static_cast<std::size_t>(-1)
      && !has_error;

    return parsed;
  }

  PatchResult applyWdtEdits(std::vector<std::uint8_t>& data,
                            ParsedWdt const& parsed,
                            std::uint32_t mphd_flags,
                            std::vector<std::uint32_t> const& main_flags)
  {
    PatchResult result;

    auto add_issue = [&result](IssueSeverity severity, std::string message)
    {
      result.issues.push_back(ParseIssue{severity, std::move(message)});
    };

    if (main_flags.size() != 4096)
    {
      add_issue(IssueSeverity::error,
                "applyWdtEdits needs exactly 4096 MAIN entry flags");
      return result;
    }

    if (!parsed.structurally_valid)
    {
      add_issue(IssueSeverity::error,
                "refusing to patch a WDT that did not parse cleanly");
      return result;
    }

    if (parsed.mphd_payload_offset == static_cast<std::size_t>(-1)
        || parsed.mphd_payload_offset + 4 > data.size())
    {
      add_issue(IssueSeverity::error, "MPHD payload offset is not inside the buffer");
      return result;
    }

    if (parsed.main_payload_offset == static_cast<std::size_t>(-1)
        || parsed.main_payload_offset + 4096 * 8 > data.size())
    {
      add_issue(IssueSeverity::error, "MAIN payload offset is not inside the buffer");
      return result;
    }

    writeU32(data.data() + parsed.mphd_payload_offset, mphd_flags);
    for (std::size_t i = 0; i < 4096; ++i)
      writeU32(data.data() + parsed.main_payload_offset + i * 8, main_flags[i]);

    result.applied = true;
    return result;
  }
}
