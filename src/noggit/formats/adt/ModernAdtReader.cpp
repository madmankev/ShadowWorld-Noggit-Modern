// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/formats/adt/ModernAdtReader.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace Noggit::Formats::ADT
{
  namespace
  {
    constexpr std::uint32_t fourccOf(char a, char b, char c, char d)
    {
      return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8)
           | static_cast<std::uint32_t>(static_cast<unsigned char>(d));
    }

    constexpr std::uint32_t FOURCC_MVER = fourccOf('M', 'V', 'E', 'R');
    constexpr std::uint32_t FOURCC_MHDR = fourccOf('M', 'H', 'D', 'R');
    constexpr std::uint32_t FOURCC_MCIN = fourccOf('M', 'C', 'I', 'N');
    constexpr std::uint32_t FOURCC_MCSR = fourccOf('M', 'C', 'S', 'R');
    constexpr std::uint32_t FOURCC_MCNK = fourccOf('M', 'C', 'N', 'K');
    constexpr std::uint32_t FOURCC_MTEX = fourccOf('M', 'T', 'E', 'X');
    constexpr std::uint32_t FOURCC_MMDX = fourccOf('M', 'M', 'D', 'X');
    constexpr std::uint32_t FOURCC_MMID = fourccOf('M', 'M', 'I', 'D');
    constexpr std::uint32_t FOURCC_MWMO = fourccOf('M', 'W', 'M', 'O');
    constexpr std::uint32_t FOURCC_MWID = fourccOf('M', 'W', 'I', 'D');
    constexpr std::uint32_t FOURCC_MDDF = fourccOf('M', 'D', 'D', 'F');
    constexpr std::uint32_t FOURCC_MODF = fourccOf('M', 'O', 'D', 'F');
    constexpr std::uint32_t FOURCC_MH2O = fourccOf('M', 'H', '2', 'O');
    constexpr std::uint32_t FOURCC_MFBO = fourccOf('M', 'F', 'B', 'O');
    constexpr std::uint32_t FOURCC_MTXF = fourccOf('M', 'T', 'X', 'F');

    constexpr std::uint32_t FOURCC_MCNR = fourccOf('M', 'C', 'N', 'R');
    constexpr std::uint32_t FOURCC_MDID = fourccOf('M', 'D', 'I', 'D');
    constexpr std::uint32_t FOURCC_MHID = fourccOf('M', 'H', 'I', 'D');
    constexpr std::uint32_t FOURCC_MTXP = fourccOf('M', 'T', 'X', 'P');
    constexpr std::uint32_t FOURCC_MCLY = fourccOf('M', 'C', 'L', 'Y');
    constexpr std::uint32_t FOURCC_MCAL = fourccOf('M', 'C', 'A', 'L');

    constexpr std::size_t mcnk_header_size = 128;
    constexpr std::size_t chunk_tag_size = 8;


    std::uint32_t readU32(char const* p)
    {
      std::uint32_t value = 0;
      std::memcpy(&value, p, sizeof(value));
      return value;
    }

    //! Chunk view for the diagnostics below.
    struct DiagChunk
    {
      std::uint32_t magic = 0;
      std::uint32_t size = 0;
      std::size_t offset = 0;
      std::size_t payload = 0;
    };

    //! Walk that reports instead of throwing: a truncated or over-run chunk ends
    //! the walk and the return value says whether the range was consumed exactly.
    //! Whatever was parsed before that point is still in `out`, because a file
    //! that disagrees with our assumptions is precisely the one worth describing.
    //! Set to the offset the walk broke at when it returns false.
    bool diag_walk(char const* data, std::size_t size,
                   std::size_t from, std::size_t to,
                   std::vector<DiagChunk>& out,
                   bool skip_mcnr_padding = false,
                   std::size_t* broke_at = nullptr)
    {
      if (broke_at)
        *broke_at = from;
      if (to > size || from > to)
        return false;
      std::size_t pos = from;
      while (pos + chunk_tag_size <= to)
      {
        DiagChunk chunk;
        chunk.magic = readU32(data + pos);
        chunk.size = readU32(data + pos + 4);
        chunk.offset = pos;
        chunk.payload = pos + chunk_tag_size;
        if (chunk.size > to - chunk.payload)
        {
          if (broke_at)
            *broke_at = pos;
          return false;
        }
        out.push_back(chunk);
        pos = chunk.payload + chunk.size;

        // The same unconditional skip the strict walkers try: Noggit's
        // serializer reserves 13 bytes after MCNR, retail puts the next chunk's
        // tag there.
        constexpr std::size_t padding = 13;
        if (skip_mcnr_padding && chunk.magic == FOURCC_MCNR && padding <= to - pos)
          pos += padding;
      }
      if (pos != to && broke_at)
        *broke_at = pos;
      return pos == to;
    }

    std::string hex_bytes(char const* data, std::size_t size,
                          std::size_t at, std::size_t count)
    {
      static char const* const digits = "0123456789ABCDEF";
      std::string out;
      for (std::size_t i = at; i < size && i < at + count; ++i)
      {
        if (i != at)
          out += ' ';
        auto const byte = static_cast<unsigned char>(data[i]);
        out += digits[byte >> 4];
        out += digits[byte & 0xF];
      }
      return out;
    }

    bool isKnownTopLevelChunk(std::uint32_t fourcc)
    {
      switch (fourcc)
      {
        case FOURCC_MVER:
        case FOURCC_MHDR:
        case FOURCC_MCIN:
        case FOURCC_MCSR:
        case FOURCC_MCNK:
        case FOURCC_MTEX:
        case FOURCC_MMDX:
        case FOURCC_MMID:
        case FOURCC_MWMO:
        case FOURCC_MWID:
        case FOURCC_MDDF:
        case FOURCC_MODF:
        case FOURCC_MH2O:
        case FOURCC_MFBO:
        case FOURCC_MTXF:
        // TEX0-only chunks are known too, so scanning a TEX0 through the
        // generic walker does not report them as unknown.
        case FOURCC_MDID:
        case FOURCC_MHID:
        case FOURCC_MTXP:
          return true;
        default:
          return false;
      }
    }

    //! Walk one MCNK payload's subchunks by size, reporting anomalies.
    void validate_mcnk_subchunks(char const* payload,
                                 std::size_t payload_size,
                                 bool has_header,
                                 std::size_t chunk_index,
                                 AdtScanOptions const& options,
                                 AdtScanResult& result)
    {
      auto add_issue = [&result](AdtIssueSeverity severity, std::string message)
      {
        result.issues.push_back(AdtIssue{severity, std::move(message)});
      };

      std::size_t pos = has_header ? mcnk_header_size : 0;

      if (pos > payload_size)
      {
        ++result.mcnk_anomalies;
        if (result.mcnk_anomalies <= options.max_subchunk_issue_messages)
        {
          std::ostringstream message;
          message << options.label << " MCNK[" << chunk_index << "] is smaller than its "
                  << mcnk_header_size << "-byte header (size=" << payload_size << ")";
          add_issue(AdtIssueSeverity::error, message.str());
        }
        return;
      }

      while (pos + 8 <= payload_size)
      {
        std::uint32_t const magic = readU32(payload + pos);
        std::uint32_t const declared_size = readU32(payload + pos + 4);

        auto const data_pos = pos + 8;
        auto const remaining = payload_size - data_pos;
        if (declared_size > remaining)
        {
          ++result.mcnk_anomalies;
          if (result.mcnk_anomalies <= options.max_subchunk_issue_messages)
          {
            std::ostringstream message;
            message << options.label << " MCNK[" << chunk_index << "] subchunk "
                    << adtChunkName(magic) << " declares " << declared_size
                    << " bytes with only " << remaining << " remaining (offset=" << pos << ")";
            add_issue(AdtIssueSeverity::error, message.str());
          }
          return;
        }

        pos = data_pos + declared_size;
      }

      if (pos != payload_size)
      {
        ++result.mcnk_anomalies;
        if (result.mcnk_anomalies <= options.max_subchunk_issue_messages)
        {
          std::ostringstream message;
          message << options.label << " MCNK[" << chunk_index << "] leaves "
                  << (payload_size - pos) << " trailing byte(s) after the subchunk walk";
          add_issue(AdtIssueSeverity::error, message.str());
        }
      }
    }
  }

  McinSizeConvention detectMcinSizeConvention(char const* data, std::size_t size)
  {
    std::vector<DiagChunk> top;
    if (!diag_walk(data, size, 0, size, top))
      return McinSizeConvention::unknown;

    DiagChunk const* mcin = nullptr;
    std::vector<DiagChunk const*> mcnks;
    for (auto const& chunk : top)
    {
      if (chunk.magic == FOURCC_MCIN)
        mcin = &chunk;
      else if (chunk.magic == FOURCC_MCNK)
        mcnks.push_back(&chunk);
    }

    if (!mcin || mcnks.empty() || mcin->size < mcnks.size() * sizeof(ENTRY_MCIN))
      return McinSizeConvention::unknown;

    std::size_t payload_only = 0;
    std::size_t plus_tag = 0;
    std::size_t neither = 0;
    for (std::size_t i = 0; i < mcnks.size(); ++i)
    {
      std::uint32_t const stored = readU32(
        data + mcin->payload + i * sizeof(ENTRY_MCIN) + offsetof(ENTRY_MCIN, size));
      if (stored == mcnks[i]->size)
        ++payload_only;
      else if (stored == mcnks[i]->size + chunk_tag_size)
        ++plus_tag;
      else
        ++neither;
    }

    if (neither || (payload_only && plus_tag))
      return McinSizeConvention::inconsistent;
    if (plus_tag)
      return McinSizeConvention::payload_plus_tag;
    if (payload_only)
      return McinSizeConvention::payload_only;
    return McinSizeConvention::unknown;
  }

  std::string mcinSizeConventionName(McinSizeConvention convention)
  {
    switch (convention)
    {
      case McinSizeConvention::payload_only: return "payload only";
      case McinSizeConvention::payload_plus_tag: return "payload + 8 byte tag";
      case McinSizeConvention::inconsistent: return "inconsistent";
      case McinSizeConvention::unknown:
      default: return "unknown";
    }
  }

  std::string describeAdtPart(char const* data, std::size_t size,
                              char const* label, bool mcnk_has_header)
  {
    std::ostringstream out;
    out << label << ": " << size << " byte(s)";
    if (!size)
      return out.str();

    std::vector<DiagChunk> top;
    bool const walked = diag_walk(data, size, 0, size, top);

    for (auto const& chunk : top)
    {
      if (chunk.magic == FOURCC_MVER && chunk.size >= 4)
      {
        out << ", MVER=" << readU32(data + chunk.payload);
        break;
      }
    }

    out << ", chunks=[";
    bool first = true;
    std::size_t mcnk_count = 0;
    DiagChunk const* first_mcnk = nullptr;
    for (auto const& chunk : top)
    {
      if (chunk.magic == FOURCC_MCNK)
      {
        ++mcnk_count;
        if (!first_mcnk)
          first_mcnk = &chunk;
        continue;
      }
      if (!first)
        out << " ";
      first = false;
      out << adtChunkName(chunk.magic) << "(" << chunk.size << ")";
    }
    if (mcnk_count)
    {
      if (!first)
        out << " ";
      out << "MCNK x" << mcnk_count;
    }
    out << "]";

    if (first_mcnk)
    {
      std::size_t const sub_from =
        first_mcnk->payload + (mcnk_has_header ? mcnk_header_size : 0);
      std::size_t const sub_to = first_mcnk->payload + first_mcnk->size;
      if (sub_from <= sub_to && sub_to <= size)
      {
        std::vector<DiagChunk> subs;
        diag_walk(data, size, sub_from, sub_to, subs);
        out << ", MCNK[0] subs=[";
        bool first_sub = true;
        for (auto const& sub : subs)
        {
          if (!first_sub)
            out << " ";
          first_sub = false;
          out << adtChunkName(sub.magic) << "(" << sub.size << ")";
        }
        out << "]";
      }
    }

    if (!walked)
      out << ", top-level walk did not end on a chunk boundary";

    return out.str();
  }

  std::string describeFirstBadMcnk(char const* data, std::size_t size,
                                   bool mcnk_has_header)
  {
    std::ostringstream out;

    std::vector<DiagChunk> top;
    std::size_t top_break = 0;
    bool const top_ok = diag_walk(data, size, 0, size, top, false, &top_break);
    if (!top_ok)
    {
      out << "top-level walk broke at offset " << top_break << " (bytes "
          << hex_bytes(data, size, top_break, 16) << "), after "
          << top.size() << " chunk(s); no MCNK could be examined";
      return out.str();
    }

    std::size_t index = 0;
    std::size_t examined = 0;
    std::size_t without_padding = 0;
    std::size_t with_padding = 0;
    for (auto const& chunk : top)
    {
      if (chunk.magic != FOURCC_MCNK)
        continue;

      std::size_t const current = index++;
      std::size_t const sub_from =
        chunk.payload + (mcnk_has_header ? mcnk_header_size : 0);
      std::size_t const sub_to = chunk.payload + chunk.size;

      if (sub_from > sub_to)
      {
        out << "MCNK " << current << " at offset " << chunk.offset << " declares size "
            << chunk.size << ", too small to hold its "
            << (mcnk_has_header ? "128 byte header" : "subchunks");
        return out.str();
      }

      ++examined;

      // The two shapes cannot be told apart by inspecting the bytes after MCNR,
      // so try both and keep whichever consumes the payload exactly. Reporting
      // the tally is the point: a file that needs the padding skipped in every
      // MCNK is Noggit-serialized, one that never needs it is retail, and one
      // that needs it in some and not others is neither - which is worth knowing
      // before trusting anything downstream of it.
      std::vector<DiagChunk> subs;
      std::size_t broke_at = 0;
      bool const fits_without = diag_walk(data, size, sub_from, sub_to, subs, false, &broke_at);
      if (fits_without)
      {
        ++without_padding;
        continue;
      }

      std::vector<DiagChunk> padded;
      std::size_t padded_break = 0;
      if (diag_walk(data, size, sub_from, sub_to, padded, true, &padded_break))
      {
        ++with_padding;
        continue;
      }

      out << "MCNK " << current << " at offset " << chunk.offset << " (declared size "
          << chunk.size << ", subchunks span " << sub_from << ".." << sub_to
          << ") is malformed under both MCNR padding conventions. Without padding"
             " it broke at offset " << broke_at << ", bytes there: "
          << hex_bytes(data, size, broke_at, 16) << ", having parsed:";
      if (subs.empty())
        out << " none";
      for (auto const& sub : subs)
      {
        out << " " << adtChunkName(sub.magic) << "(offset " << sub.offset
            << ", size " << sub.size << ")";
      }
      out << ". With padding it broke at offset " << padded_break << ", bytes there: "
          << hex_bytes(data, size, padded_break, 16) << ", having parsed:";
      if (padded.empty())
        out << " none";
      for (auto const& sub : padded)
      {
        out << " " << adtChunkName(sub.magic) << "(offset " << sub.offset
            << ", size " << sub.size << ")";
      }
      return out.str();
    }

    out << "all " << examined << " MCNK subchunk walk(s) consumed their payload exactly ("
        << without_padding << " with no MCNR padding, " << with_padding
        << " needing the 13 byte padding skipped)";
    return out.str();
  }

  std::string adtChunkName(std::uint32_t fourcc)
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

  std::string adtLayoutName(AdtLayout layout)
  {
    switch (layout)
    {
      case AdtLayout::legacy_single_file: return "legacy single-file ADT";
      case AdtLayout::modern_split_root: return "modern split ROOT ADT";
      default: return "unusable ADT";
    }
  }

  std::string severityName(AdtIssueSeverity severity)
  {
    switch (severity)
    {
      case AdtIssueSeverity::info: return "info";
      case AdtIssueSeverity::warning: return "warning";
      default: return "error";
    }
  }

  AdtScanResult scanAdt(char const* data, std::size_t size, AdtScanOptions const& options)
  {
    static_assert(sizeof(MHDR) == 0x40, "MHDR must stay 64 bytes");
    static_assert(sizeof(MapChunkHeader) == 128, "MapChunkHeader must stay 128 bytes");
    static_assert(sizeof(ENTRY_MDDF) == 36, "ENTRY_MDDF must stay 36 bytes");
    static_assert(sizeof(ENTRY_MODF) == 64, "ENTRY_MODF must stay 64 bytes");

    AdtScanResult result;

    auto add_issue = [&result](AdtIssueSeverity severity, std::string message)
    {
      result.issues.push_back(AdtIssue{severity, std::move(message)});
    };

    if (size == 0)
    {
      add_issue(AdtIssueSeverity::error, options.label + " file is empty");
      return result;
    }

    std::size_t pos = 0;
    while (pos + 8 <= size)
    {
      std::uint32_t const magic = readU32(data + pos);
      std::uint32_t const declared_size = readU32(data + pos + 4);
      std::size_t const payload = pos + 8;

      if (declared_size > size - payload)
      {
        std::ostringstream message;
        message << options.label << " top-level chunk " << adtChunkName(magic)
                << " at offset " << pos << " declares " << declared_size
                << " bytes with only " << (size - payload) << " remaining";
        add_issue(AdtIssueSeverity::error, message.str());
        return result;
      }

      char const* payload_ptr = data + payload;

      switch (magic)
      {
        case FOURCC_MVER:
        {
          result.mver_present = true;
          if (declared_size >= 4)
            result.mver = readU32(payload_ptr);
          if (declared_size != 4)
          {
            std::ostringstream message;
            message << options.label << " MVER chunk has unexpected size " << declared_size
                    << " (expected 4)";
            add_issue(AdtIssueSeverity::warning, message.str());
          }
          if (result.mver != 18)
          {
            std::ostringstream message;
            message << options.label << " has unsupported ADT version " << result.mver
                    << " (expected 18)";
            add_issue(AdtIssueSeverity::error, message.str());
          }
          break;
        }
        case FOURCC_MHDR:
        {
          result.has_mhdr = true;
          if (declared_size < sizeof(MHDR))
          {
            std::ostringstream message;
            message << options.label << " MHDR chunk is " << declared_size << " bytes (expected "
                    << sizeof(MHDR) << ")";
            add_issue(AdtIssueSeverity::warning, message.str());
          }
          std::memcpy(&result.mhdr, payload_ptr, std::min<std::size_t>(declared_size, sizeof(MHDR)));
          break;
        }
        case FOURCC_MCIN: result.has_mcin = true; break;
        case FOURCC_MCSR: result.has_mcsr = true; break;
        case FOURCC_MTEX: result.has_mtex = true; break;
        case FOURCC_MMDX: result.has_mmdx = true; break;
        case FOURCC_MMID: result.has_mmid = true; break;
        case FOURCC_MWMO: result.has_mwmo = true; break;
        case FOURCC_MWID: result.has_mwid = true; break;
        case FOURCC_MDDF: result.has_mddf = true; break;
        case FOURCC_MODF: result.has_modf = true; break;
        case FOURCC_MH2O: result.has_mh2o = true; break;
        case FOURCC_MTXF: result.has_mtxf = true; break;
        case FOURCC_MFBO:
        {
          result.has_mfbo = true;
          if (declared_size == sizeof(MfboExtents))
          {
            std::memcpy(result.mfbo.maximum, payload_ptr, 18);
            std::memcpy(result.mfbo.minimum, payload_ptr + 18, 18);
            result.mfbo_valid = true;
          }
          else
          {
            std::ostringstream message;
            message << options.label << " MFBO chunk has unexpected size " << declared_size
                    << " (expected " << sizeof(MfboExtents) << ")";
            add_issue(AdtIssueSeverity::warning, message.str());
          }
          break;
        }
        case FOURCC_MCNK:
        {
          if (options.validate_mcnk_subchunks)
          {
            validate_mcnk_subchunks(payload_ptr, declared_size, options.mcnk_has_header,
                                    result.mcnk_offsets.size(), options, result);
          }
          result.mcnk_offsets.push_back(pos);
          break;
        }
        default:
        {
          if (!isKnownTopLevelChunk(magic))
            ++result.unknown_chunks[magic];
          break;
        }
      }

      pos = payload + declared_size;
    }

    if (result.mcnk_anomalies > options.max_subchunk_issue_messages)
    {
      std::ostringstream message;
      message << options.label << " has " << result.mcnk_anomalies
              << " MCNK subchunk anomalies in total (first "
              << options.max_subchunk_issue_messages << " reported above)";
      add_issue(AdtIssueSeverity::error, message.str());
    }

    if (!result.mver_present)
      add_issue(AdtIssueSeverity::error, options.label + " file has no MVER chunk");

    if (pos != size)
    {
      std::ostringstream message;
      message << options.label << " scan stopped at " << pos << " of " << size
              << " bytes (incomplete chunk header or trailing garbage)";
      add_issue(AdtIssueSeverity::error, message.str());
      return result;
    }

    result.structurally_valid = true;
    return result;
  }

  AdtLayout classifyAdtLayout(AdtScanResult const& scan)
  {
    // The name-list chunks only exist in WotLK-style single files; modern
    // split ROOT parts never carry MTEX/MMDX/MWMO/MDDF/MODF.
    if (scan.has_mtex || scan.has_mmdx || scan.has_mwmo
        || scan.has_mddf || scan.has_modf || scan.has_mmid)
      return AdtLayout::legacy_single_file;

    if (scan.structurally_valid && scan.mver == 18 && scan.mcnk_offsets.size() == 256)
      return AdtLayout::modern_split_root;

    return AdtLayout::unusable;
  }

  Tex0ParseResult parseTex0(char const* data, std::size_t size)
  {
    Tex0ParseResult result;

    auto add_issue = [&result](AdtIssueSeverity severity, std::string message)
    {
      result.issues.push_back(AdtIssue{severity, std::move(message)});
    };

    auto& out = result.data;
    out.chunks.reserve(256);

    std::size_t pos = 0;
    while (pos + 8 <= size)
    {
      std::uint32_t const magic = readU32(data + pos);
      std::uint32_t const chunk_size = readU32(data + pos + 4);
      std::size_t const payload = pos + 8;

      if (chunk_size > size - payload)
      {
        add_issue(AdtIssueSeverity::error,
                  "TEX0 contains a truncated top-level chunk ("
                  + adtChunkName(magic) + " at offset " + std::to_string(pos) + ")");
        return result;
      }

      if (magic == FOURCC_MDID || magic == FOURCC_MHID)
      {
        if (magic == FOURCC_MHID)
          out.has_height_ids_chunk = true;
        auto& ids = (magic == FOURCC_MDID) ? out.diffuse_ids : out.height_ids;
        if (chunk_size % 4 != 0)
        {
          add_issue(AdtIssueSeverity::error,
                    "TEX0 texture-ID chunk " + adtChunkName(magic) + " is not uint32 aligned");
          return result;
        }
        ids.resize(chunk_size / 4);
        if (chunk_size)
          std::memcpy(ids.data(), data + payload, chunk_size);
      }
      else if (magic == FOURCC_MTXP)
      {
        if (chunk_size % 16 != 0)
        {
          add_issue(AdtIssueSeverity::error, "TEX0 MTXP is not aligned to 16-byte entries");
          return result;
        }
        out.texture_parameters.resize(chunk_size / 16);
        for (std::size_t i = 0; i < out.texture_parameters.size(); ++i)
        {
          auto const entry = payload + i * 16;
          auto& parameters = out.texture_parameters[i];
          parameters.flags = readU32(data + entry);
          std::memcpy(&parameters.height_scale, data + entry + 4, 4);
          std::memcpy(&parameters.height_offset, data + entry + 8, 4);
        }
      }
      else if (magic == FOURCC_MCNK)
      {
        ModernTexChunk tex_chunk;
        std::size_t sub = payload;
        auto const chunk_end = payload + chunk_size;
        while (sub + 8 <= chunk_end)
        {
          std::uint32_t const sub_magic = readU32(data + sub);
          std::uint32_t const sub_size = readU32(data + sub + 4);
          std::size_t const sub_data = sub + 8;

          if (sub_size > chunk_end - sub_data)
          {
            add_issue(AdtIssueSeverity::error,
                      "TEX0 MCNK[" + std::to_string(out.chunks.size())
                      + "] contains a truncated subchunk");
            return result;
          }

          if (sub_magic == FOURCC_MCLY)
          {
            // WMO-only Shadowlands tiles (including Torghast) legitimately
            // contain an empty MCLY in every TEX0 MCNK.
            if (sub_size % 16 != 0)
            {
              add_issue(AdtIssueSeverity::error, "TEX0 MCLY has an invalid size");
              return result;
            }

            auto const entry_count = sub_size / 16;
            if (entry_count > static_cast<std::size_t>(MAX_TEXTURE_LAYERS))
            {
              add_issue(AdtIssueSeverity::warning,
                        "TEX0 MCNK[" + std::to_string(out.chunks.size()) + "] has "
                        + std::to_string(entry_count) + " MCLY layers (max "
                        + std::to_string(MAX_TEXTURE_LAYERS) + "); the rest are ignored");
            }
            auto const load_count =
              std::min<std::size_t>(entry_count, static_cast<std::size_t>(MAX_TEXTURE_LAYERS));
            tex_chunk.layers.reserve(load_count);

            for (std::size_t i = 0; i < load_count; ++i)
            {
              ModernTextureLayer layer;
              auto const entry = sub_data + i * 16;
              layer.texture_id = readU32(data + entry);
              layer.flags = readU32(data + entry + 4);
              layer.alpha_offset = readU32(data + entry + 8);
              layer.effect_id = readU32(data + entry + 12);
              tex_chunk.layers.push_back(layer);
            }
          }
          else if (sub_magic == FOURCC_MCAL)
          {
            tex_chunk.mcal.assign(
              reinterpret_cast<std::uint8_t const*>(data + sub_data),
              reinterpret_cast<std::uint8_t const*>(data + sub_data + sub_size));
          }

          sub = sub_data + sub_size;
        }

        out.chunks.push_back(std::move(tex_chunk));
      }

      pos = payload + chunk_size;
    }

    if (out.chunks.size() != 256)
    {
      add_issue(AdtIssueSeverity::error,
                "TEX0 texture decoder expected exactly 256 MCNK chunks, found "
                + std::to_string(out.chunks.size()));
      return result;
    }

    for (auto& tex_chunk : out.chunks)
    {
      for (auto& layer : tex_chunk.layers)
      {
        if (layer.texture_id >= out.diffuse_ids.size())
        {
          add_issue(AdtIssueSeverity::error,
                    "TEX0 MCLY references a diffuse texture outside MDID (texture id "
                    + std::to_string(layer.texture_id) + ", MDID holds "
                    + std::to_string(out.diffuse_ids.size()) + ")");
          return result;
        }
        layer.file_data_id = out.diffuse_ids[layer.texture_id];
        layer.has_height_id_table = out.has_height_ids_chunk;
        if (layer.texture_id < out.height_ids.size())
          layer.height_file_data_id = out.height_ids[layer.texture_id];
      }
    }

    // MTXP is not guaranteed to precede the MCNK chunks inside TEX0, so the
    // parameters are applied to the layers once the whole file is known.
    for (auto& tex_chunk : out.chunks)
    {
      for (auto& layer : tex_chunk.layers)
      {
        if (layer.texture_id < out.texture_parameters.size())
        {
          auto const& parameters = out.texture_parameters[layer.texture_id];
          layer.has_mtxp = true;
          // Flag bit 0 disables the height/specular load, and the client also
          // treats the default combination (scale 0, offset 1) as "no height
          // texture" - loading one anyway would waste memory and uploads.
          layer.disable_height_texture = (parameters.flags & 0x1u) != 0
            || (parameters.height_scale == 0.0f && parameters.height_offset == 1.0f);
          layer.mtxp_uv_scale = (parameters.flags >> 4) & 0x0Fu;
          layer.mtxp_height_scale = parameters.height_scale;
          layer.mtxp_height_offset = parameters.height_offset;
        }
      }
    }

    result.ok = true;
    return result;
  }

  bool decodeModernAlpha(ModernTexChunk const& tex_chunk,
                         ModernTextureLayer const& layer,
                         std::size_t layer_index,
                         std::vector<std::uint8_t>& output)
  {
    output.clear();
    output.reserve(64 * 64);

    if (!(layer.flags & FLAG_USE_ALPHA))
      return false;
    if (layer.alpha_offset >= tex_chunk.mcal.size())
      return false;

    auto const* input = tex_chunk.mcal.data() + layer.alpha_offset;
    auto const* end = tex_chunk.mcal.data() + tex_chunk.mcal.size();

    if (layer.flags & FLAG_ALPHA_COMPRESSED)
    {
      while (output.size() < 4096 && input < end)
      {
        std::uint8_t const control = *input++;
        std::size_t const count = control & 0x7F;
        bool const fill = (control & 0x80) != 0;
        if (!count)
          continue;

        auto const remaining = 4096 - output.size();
        auto const copy_count = std::min<std::size_t>(count, remaining);

        if (fill)
        {
          if (input >= end)
            return false;
          std::uint8_t const value = *input++;
          output.insert(output.end(), copy_count, value);
        }
        else
        {
          if (static_cast<std::size_t>(end - input) < count)
            return false;
          output.insert(output.end(), input, input + copy_count);
          input += count;
        }
      }
      return output.size() == 4096;
    }

    std::size_t stored_size = static_cast<std::size_t>(end - input);
    for (std::size_t next_index = layer_index + 1;
         next_index < tex_chunk.layers.size(); ++next_index)
    {
      auto const& next_layer = tex_chunk.layers[next_index];
      if (!(next_layer.flags & FLAG_USE_ALPHA))
        continue;
      if (next_layer.alpha_offset > layer.alpha_offset
          && next_layer.alpha_offset <= tex_chunk.mcal.size())
      {
        stored_size = next_layer.alpha_offset - layer.alpha_offset;
        break;
      }
    }

    if (stored_size >= 4096)
    {
      output.assign(input, input + 4096);
      return true;
    }

    if (stored_size >= 2048)
    {
      output.resize(4096);
      for (std::size_t byte_index = 0; byte_index < 2048; ++byte_index)
      {
        std::uint8_t const packed = input[byte_index];
        std::uint8_t const low = packed & 0x0F;
        std::uint8_t const high = packed >> 4;
        output[byte_index * 2] = low | (low << 4);
        output[byte_index * 2 + 1] = high | (high << 4);
      }
      return true;
    }

    return false;
  }

  Obj0ParseResult parseObj0(char const* data, std::size_t size)
  {
    Obj0ParseResult result;

    auto add_issue = [&result](AdtIssueSeverity severity, std::string message)
    {
      result.issues.push_back(AdtIssue{severity, std::move(message)});
    };

    std::size_t pos = 0;
    while (pos + 8 <= size)
    {
      std::uint32_t const magic = readU32(data + pos);
      std::uint32_t const chunk_size = readU32(data + pos + 4);
      std::size_t const payload = pos + 8;

      if (chunk_size > size - payload)
      {
        add_issue(AdtIssueSeverity::error,
                  "OBJ0 contains a truncated top-level chunk ("
                  + adtChunkName(magic) + " at offset " + std::to_string(pos) + ")");
        return result;
      }

      if (magic == FOURCC_MDDF)
      {
        if (chunk_size % sizeof(ENTRY_MDDF) != 0)
        {
          add_issue(AdtIssueSeverity::error,
                    "OBJ0 MDDF size is not aligned to ENTRY_MDDF ("
                    + std::to_string(chunk_size) + " bytes)");
          return result;
        }
        auto const count = chunk_size / sizeof(ENTRY_MDDF);
        result.m2_placements.resize(count);
        if (count)
          std::memcpy(result.m2_placements.data(), data + payload, chunk_size);
      }
      else if (magic == FOURCC_MODF)
      {
        if (chunk_size % sizeof(ENTRY_MODF) != 0)
        {
          add_issue(AdtIssueSeverity::error,
                    "OBJ0 MODF size is not aligned to ENTRY_MODF");
          return result;
        }
        auto const count = chunk_size / sizeof(ENTRY_MODF);
        result.wmo_placements.resize(count);
        if (count)
          std::memcpy(result.wmo_placements.data(), data + payload, chunk_size);
      }

      pos = payload + chunk_size;
    }

    result.ok = true;
    return result;
  }
}
