// Two-way WotLK <-> modern map conversion: orchestrator, WDT/WDL builders,
// FileDataID allocation and the manifest (see docs/MAP_CONVERSION.md).
// Qt-free and logging-free like the rest of the conversion module.

#include <noggit/formats/conversion/MapConversion.hpp>

#include <noggit/MapHeaders.h>
#include <noggit/formats/wdt/WdtReader.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace Noggit::Formats::Conversion
{
  using Noggit::Formats::WDT::MaidEntry;
  using Noggit::Formats::WDT::ParsedWdt;

  //! Named (not anonymous) so unity builds cannot merge these helpers
  //! with the identically shaped ones in AdtConversion.cpp.
  namespace map_wdt_wdl_detail
  {
    constexpr std::uint32_t FC_MVER = (std::uint32_t('M') << 24) | (std::uint32_t('V') << 16) | (std::uint32_t('E') << 8) | std::uint32_t('R');
    constexpr std::uint32_t FC_MPHD = (std::uint32_t('M') << 24) | (std::uint32_t('P') << 16) | (std::uint32_t('H') << 8) | std::uint32_t('D');
    constexpr std::uint32_t FC_MAIN = (std::uint32_t('M') << 24) | (std::uint32_t('A') << 16) | (std::uint32_t('I') << 8) | std::uint32_t('N');
    constexpr std::uint32_t FC_MAID = (std::uint32_t('M') << 24) | (std::uint32_t('A') << 16) | (std::uint32_t('I') << 8) | std::uint32_t('D');
    constexpr std::uint32_t FC_MWMO = (std::uint32_t('M') << 24) | (std::uint32_t('W') << 16) | (std::uint32_t('M') << 8) | std::uint32_t('O');
    constexpr std::uint32_t FC_MWID = (std::uint32_t('M') << 24) | (std::uint32_t('W') << 16) | (std::uint32_t('I') << 8) | std::uint32_t('D');
    constexpr std::uint32_t FC_MODF = (std::uint32_t('M') << 24) | (std::uint32_t('O') << 16) | (std::uint32_t('D') << 8) | std::uint32_t('F');
    constexpr std::uint32_t FC_MAOF = (std::uint32_t('M') << 24) | (std::uint32_t('A') << 16) | (std::uint32_t('O') << 8) | std::uint32_t('F');
    constexpr std::uint32_t FC_MARE = (std::uint32_t('M') << 24) | (std::uint32_t('A') << 16) | (std::uint32_t('R') << 8) | std::uint32_t('E');
    constexpr std::uint32_t FC_MAHO = (std::uint32_t('M') << 24) | (std::uint32_t('A') << 16) | (std::uint32_t('H') << 8) | std::uint32_t('O');
    constexpr std::uint32_t FC_MLMD = (std::uint32_t('M') << 24) | (std::uint32_t('L') << 16) | (std::uint32_t('M') << 8) | std::uint32_t('D');
    constexpr std::uint32_t FC_MLMX = (std::uint32_t('M') << 24) | (std::uint32_t('L') << 16) | (std::uint32_t('M') << 8) | std::uint32_t('X');
    constexpr std::uint32_t FC_MLDD = (std::uint32_t('M') << 24) | (std::uint32_t('L') << 16) | (std::uint32_t('D') << 8) | std::uint32_t('D');
    constexpr std::uint32_t FC_MLDX = (std::uint32_t('M') << 24) | (std::uint32_t('L') << 16) | (std::uint32_t('D') << 8) | std::uint32_t('X');

    //! MPHD flag: modern WDTs carry MAID (8.1+ clients locate ADTs by
    //! FileDataID).
    constexpr std::uint32_t MPHD_FLAG_HAS_MAID = 0x200;
    //! MPHD flag: height texturing (MTXP) - has no WotLK equivalent.
    constexpr std::uint32_t MPHD_FLAG_HEIGHT_TEXTURES = 0x80;
    //! MODF flag: nameID is a FileDataID (Legion+).
    constexpr std::uint32_t MODF_FLAG_NAME_IS_FDID = 0x8;

    constexpr std::size_t MARE_PAYLOAD_SIZE = 1090; // 17*17*2 + 16*16*2
    constexpr std::size_t MAHO_PAYLOAD_SIZE = 32;   // 16 * 2



    //! Sizes and every other numeric field are plain little-endian.
    std::uint32_t readU32(std::uint8_t const* p)
    {
      return static_cast<std::uint32_t>(p[0])
           | (static_cast<std::uint32_t>(p[1]) << 8)
           | (static_cast<std::uint32_t>(p[2]) << 16)
           | (static_cast<std::uint32_t>(p[3]) << 24);
    }

    //! Fourccs live on disk as ordinary little-endian u32s whose value is
    //! the MSB-first spelling ('M' in the high byte) - exactly what
    //! SetChunkHeader stores and what ModernAdtReader/WdtReader compare
    //! against. The on-disk bytes therefore spell the tag backwards.
    std::uint32_t readMagic(std::uint8_t const* p)
    {
      return readU32(p);
    }
    struct RawChunk
    {
      std::uint32_t magic = 0;
      std::uint32_t size = 0;
      std::size_t payload = 0;
      std::size_t start() const { return payload - 8; }
    };

    bool walkChunks(std::uint8_t const* data,
                    std::size_t from,
                    std::size_t to,
                    std::vector<RawChunk>& out,
                    std::vector<ConversionIssue>& issues,
                    char const* context)
    {
      std::size_t pos = from;
      while (pos + 8 <= to)
      {
        RawChunk chunk;
        chunk.magic = readMagic(data + pos);
        chunk.size = readU32(data + pos + 4);
        chunk.payload = pos + 8;
        if (chunk.size > to - chunk.payload)
        {
          std::ostringstream message;
          message << context << ": truncated chunk at offset " << pos
                  << " (declared size " << chunk.size << ")";
          issues.push_back({ConversionIssue::Severity::error, message.str()});
          return false;
        }
        out.push_back(chunk);
        pos = chunk.payload + chunk.size;
      }
      return true;
    }

    RawChunk const* findChunk(std::vector<RawChunk> const& chunks, std::uint32_t magic)
    {
      auto const it = std::find_if(chunks.begin(), chunks.end(),
                                   [magic](RawChunk const& c) { return c.magic == magic; });
      return it == chunks.end() ? nullptr : &*it;
    }

    struct ByteSink
    {
      std::vector<std::uint8_t> bytes;

      void u32(std::uint32_t v)
      {
        bytes.push_back(static_cast<std::uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
      }
      void f32(float v)
      {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
      }
      void raw(void const* data, std::size_t size)
      {
        auto const* p = reinterpret_cast<std::uint8_t const*>(data);
        bytes.insert(bytes.end(), p, p + size);
      }
      void zeros(std::size_t size) { bytes.insert(bytes.end(), size, 0); }
      void cstring(std::string const& s) { raw(s.c_str(), s.size() + 1); }
      std::size_t position() const { return bytes.size(); }
      void patchU32(std::size_t at, std::uint32_t v)
      {
        bytes[at + 0] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        bytes[at + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        bytes[at + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
      }
      //! Chunk header: magic and size are both plain little-endian u32s;
      //! the magic value is the MSB-first fourcc spelling (see readMagic).
      void chunkHeader(std::uint32_t magic, std::uint32_t size)
      {
        u32(magic);
        u32(size);
      }
      void chunkFrom(std::uint32_t magic, void const* payload, std::size_t size)
      {
        chunkHeader(magic, static_cast<std::uint32_t>(size));
        raw(payload, size);
      }
    };

    float readFloat(std::uint8_t const* p)
    {
      std::uint32_t const bits = readU32(p);
      float value;
      std::memcpy(&value, &bits, 4);
      return value;
    }

    void warn(std::vector<ConversionIssue>& issues, std::string message)
    {
      issues.push_back({ConversionIssue::Severity::warning, std::move(message)});
    }

    void fail(std::vector<ConversionIssue>& issues, std::string message)
    {
      issues.push_back({ConversionIssue::Severity::error, std::move(message)});
    }

    std::string lower(std::string s)
    {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    std::string withBackslashes(std::string path)
    {
      for (char& c : path)
        if (c == '/')
          c = '\\';
      return path;
    }

    std::string tileSuffix(unsigned x, unsigned y)
    {
      return "_" + std::to_string(x) + "_" + std::to_string(y);
    }

    // ------------------------------------------------------------------
    // WDL tile records (MARE/MAHO reached through MAOF absolute offsets)
    // ------------------------------------------------------------------

    struct WdlTileRecord
    {
      std::vector<std::uint8_t> mare; // 1090 bytes
      std::vector<std::uint8_t> maho; // 32 bytes, optional
    };

    //! Collect the MAOF table and the MARE/MAHO records it points at.
    bool readWdlTiles(std::vector<std::uint8_t> const& bytes,
                      std::vector<RawChunk> const& top,
                      std::array<WdlTileRecord, 4096>& tiles,
                      std::vector<ConversionIssue>& issues,
                      char const* context)
    {
      RawChunk const* maof = findChunk(top, FC_MAOF);
      if (!maof)
        return true; // no low-res height data is legal
      if (maof->size < 4096 * 4)
      {
        std::ostringstream message;
        message << context << ": MAOF chunk is too small (" << maof->size << " bytes)";
        fail(issues, message.str());
        return false;
      }

      for (std::size_t i = 0; i < 4096; ++i)
      {
        std::uint32_t const offset = readU32(bytes.data() + maof->payload + i * 4);
        if (!offset)
          continue;
        if (offset + 8 > bytes.size() || readMagic(bytes.data() + offset) != FC_MARE)
        {
          std::ostringstream message;
          message << context << ": MAOF entry " << i << " does not point at a MARE chunk;"
                  << " tile skipped";
          warn(issues, message.str());
          continue;
        }
        std::uint32_t const mare_size = readU32(bytes.data() + offset + 4);
        if (mare_size != MARE_PAYLOAD_SIZE || offset + 8 + mare_size > bytes.size())
        {
          std::ostringstream message;
          message << context << ": MARE chunk at " << offset << " has unexpected size "
                  << mare_size << "; tile skipped";
          warn(issues, message.str());
          continue;
        }
        WdlTileRecord record;
        record.mare.assign(bytes.begin() + offset + 8,
                           bytes.begin() + offset + 8 + mare_size);

        std::size_t const after_mare = offset + 8 + mare_size;
        if (after_mare + 8 <= bytes.size() && readMagic(bytes.data() + after_mare) == FC_MAHO)
        {
          std::uint32_t const maho_size = readU32(bytes.data() + after_mare + 4);
          if (maho_size == MAHO_PAYLOAD_SIZE && after_mare + 8 + maho_size <= bytes.size())
            record.maho.assign(bytes.begin() + after_mare + 8,
                               bytes.begin() + after_mare + 8 + maho_size);
        }
        tiles[i] = std::move(record);
      }
      return true;
    }

    void emitWdlTiles(ByteSink& out,
                      std::array<WdlTileRecord, 4096> const& tiles,
                      std::vector<std::uint32_t>& maof_entries)
    {
      for (std::size_t i = 0; i < 4096; ++i)
      {
        if (tiles[i].mare.empty())
          continue;
        maof_entries[i] = static_cast<std::uint32_t>(out.position());
        out.chunkFrom(FC_MARE, tiles[i].mare.data(), tiles[i].mare.size());
        if (!tiles[i].maho.empty())
          out.chunkFrom(FC_MAHO, tiles[i].maho.data(), tiles[i].maho.size());
      }
    }

    // ------------------------------------------------------------------
    // MLMD/MLMX records (Legion+ WDL/_obj1 object layout)
    // ------------------------------------------------------------------

    struct MlmdRecord
    {
      std::uint32_t nameID = 0;
      std::uint32_t uniqueID = 0;
      float pos[3]{};
      float rot[3]{};
      std::uint16_t flags = 0;
      std::uint16_t doodadSet = 0;
      std::uint16_t nameSet = 0;
      std::uint16_t scale = 0;
    };
    static_assert(sizeof(MlmdRecord) == 40, "MLMD records are 40 bytes");

    struct MlmxRecord
    {
      float min[3]{};
      float max[3]{};
      float radius = 0.0f;
    };
    static_assert(sizeof(MlmxRecord) == 28, "MLMX records are 28 bytes");

    std::string jsonEscape(std::string const& in)
    {
      std::string out;
      out.reserve(in.size() + 8);
      for (char c : in)
      {
        switch (c)
        {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '\t': out += "\\t"; break;
          default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
              char buffer[8];
              std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
              out += buffer;
            }
            else
              out += c;
        }
      }
      return out;
    }
  }

  using namespace map_wdt_wdl_detail; // helpers shared with the public functions below

  // --------------------------------------------------------------------
  // FdidAllocator / normalize_path
  // --------------------------------------------------------------------

  FdidAllocator::FdidAllocator(std::uint32_t base)
    : _next(base ? base : 900000000u)
  {
  }

  std::uint32_t FdidAllocator::resolve(std::string const& raw_path,
                                       std::string const& role,
                                       ConversionFileSource const& source)
  {
    std::string const normalized = normalize_path(raw_path);
    auto const cached = _by_path.find(normalized);
    if (cached != _by_path.end())
      return cached->second;

    std::uint32_t fdid = 0;
    bool existed = false;
    if (source.path_to_fdid)
    {
      if (auto const known = source.path_to_fdid(normalized))
      {
        fdid = *known;
        existed = true;
      }
    }
    if (!fdid)
      fdid = _next++;

    FdidAssignment assignment;
    assignment.fdid = fdid;
    assignment.path = normalized;
    assignment.role = role;
    assignment.existed_in_client = existed;
    _assignments.push_back(assignment);
    _by_path.emplace(normalized, fdid);
    return fdid;
  }

  std::uint32_t FdidAllocator::allocate(std::string path, std::string role)
  {
    auto const cached = _by_path.find(path);
    if (cached != _by_path.end())
      return cached->second;
    FdidAssignment assignment;
    assignment.fdid = _next++;
    assignment.path = path;
    assignment.role = std::move(role);
    assignment.existed_in_client = false;
    _assignments.push_back(assignment);
    _by_path.emplace(assignment.path, assignment.fdid);
    return assignment.fdid;
  }

  void FdidAllocator::record_existing(std::uint32_t fdid, std::string path, std::string role)
  {
    // Skip duplicates: the same file resolved twice must not clutter the
    // manifest.
    for (FdidAssignment const& existing : _assignments)
      if (existing.fdid == fdid)
        return;
    FdidAssignment assignment;
    assignment.fdid = fdid;
    assignment.path = std::move(path);
    assignment.role = std::move(role);
    assignment.existed_in_client = true;
    _assignments.push_back(assignment);
    _by_path.emplace(assignment.path, fdid);
  }

  std::string normalize_path(std::string const& path)
  {
    std::string result = lower(path);
    for (char& c : result)
      if (c == '\\')
        c = '/';
    // Collapse a leading slash: listfile keys never carry one.
    while (!result.empty() && result.front() == '/')
      result.erase(result.begin());
    return result;
  }

  // --------------------------------------------------------------------
  // WDT builders
  // --------------------------------------------------------------------

  std::vector<std::uint8_t> buildModernWdt(std::vector<std::uint8_t> const& legacy_wdt,
                                           std::array<MaidEntryOut, 4096> const& tile_fdids,
                                           ConversionFileSource const& source,
                                           FdidAllocator& fdids,
                                           std::vector<ConversionIssue>& issues)
  {
    ParsedWdt const parsed = Noggit::Formats::WDT::parseWdt(legacy_wdt, false);
    if (!parsed.structurally_valid || !parsed.main_present)
    {
      fail(issues, "source WDT is not a structurally valid legacy WDT");
      for (auto const& issue : parsed.issues)
        if (issue.severity == Noggit::Formats::WDT::IssueSeverity::error)
          fail(issues, "WDT: " + issue.message);
      return {};
    }

    ByteSink out;
    out.chunkHeader(FC_MVER, 4);
    out.u32(18);

    MPHD mphd = parsed.mphd;
    mphd.flags |= MPHD_FLAG_HAS_MAID;
    for (std::uint32_t& unused : mphd.unused)
      unused = 0; // fdid columns (lgt/occ/...) stay empty for converted maps
    out.chunkHeader(FC_MPHD, sizeof(MPHD));
    out.raw(&mphd, sizeof(mphd));

    out.chunkHeader(FC_MAIN, 4096 * 8);
    for (std::size_t i = 0; i < 4096; ++i)
    {
      out.u32(parsed.main_flags[i]);
      out.u32(parsed.main_async_ids[i]);
    }

    out.chunkHeader(FC_MAID, 4096 * 32);
    for (std::size_t i = 0; i < 4096; ++i)
    {
      MaidEntryOut const& entry = tile_fdids[i];
      out.u32(entry.root_adt);
      out.u32(entry.obj0);
      out.u32(entry.obj1);
      out.u32(entry.tex0);
      out.u32(entry.lod);
      out.u32(entry.map_texture);
      out.u32(entry.map_texture_n);
      out.u32(entry.minimap);
    }

    // Global WMO: legacy MWMO+MODF becomes a single fdid-referencing MODF.
    if (parsed.modf_present && parsed.mwmo_present && !parsed.mwmo_name.empty())
    {
      std::uint32_t const fdid = fdids.resolve(parsed.mwmo_name, "wmo", source);

      ENTRY_MODF modf = parsed.modf;
      modf.nameID = fdid;
      modf.flags |= MODF_FLAG_NAME_IS_FDID;
      out.chunkHeader(FC_MODF, sizeof(ENTRY_MODF));
      out.raw(&modf, sizeof(modf));
    }

    return out.bytes;
  }

  std::vector<std::uint8_t> buildLegacyWdt(std::vector<std::uint8_t> const& modern_wdt,
                                           ConversionOptions const& options,
                                           ConversionFileSource const& source,
                                           FdidAllocator& fdids,
                                           std::vector<ConversionIssue>& issues)
  {
    (void)fdids; // name-based target: no new FileDataIDs are minted
    ParsedWdt const parsed = Noggit::Formats::WDT::parseWdt(modern_wdt, true);
    if (!parsed.structurally_valid || !parsed.main_present)
    {
      fail(issues, "source WDT is not a structurally valid modern WDT");
      for (auto const& issue : parsed.issues)
        if (issue.severity == Noggit::Formats::WDT::IssueSeverity::error)
          fail(issues, "WDT: " + issue.message);
      return {};
    }

    if (parsed.mphd.flags & MPHD_FLAG_HEIGHT_TEXTURES)
      warn(issues, "modern WDT declares height texturing (MPHD 0x80);"
                   " WotLK has no equivalent, flag cleared");

    ByteSink out;
    out.chunkHeader(FC_MVER, 4);
    out.u32(18);

    MPHD mphd = parsed.mphd;
    mphd.flags &= ~(MPHD_FLAG_HAS_MAID | MPHD_FLAG_HEIGHT_TEXTURES);
    if (options.big_alpha)
      mphd.flags |= 0x4; // legacy WDT big-alpha flag
    else
      mphd.flags &= ~0x4u;
    for (std::uint32_t& unused : mphd.unused)
      unused = 0;
    out.chunkHeader(FC_MPHD, sizeof(MPHD));
    out.raw(&mphd, sizeof(mphd));

    out.chunkHeader(FC_MAIN, 4096 * 8);
    for (std::size_t i = 0; i < 4096; ++i)
    {
      out.u32(parsed.main_flags[i]);
      out.u32(parsed.main_async_ids[i]);
    }

    // Global WMO: modern fdid MODF becomes MWMO + MWID + MODF.
    if (parsed.modf_present)
    {
      std::string path;
      if (parsed.modf_name_is_fdid)
      {
        if (source.fdid_to_path)
          path = source.fdid_to_path(parsed.modf.nameID);
        if (path.empty())
        {
          std::ostringstream message;
          message << "global WMO FileDataID " << parsed.modf.nameID
                  << " is not in the source listfile; emitted as a placeholder path";
          warn(issues, message.str());
          path = "missing\\fdid_" + std::to_string(parsed.modf.nameID) + ".wmo";
        }
      }
      else
      {
        path = parsed.mwmo_name; // already a name reference
      }

      if (!path.empty())
      {
        out.chunkHeader(FC_MWMO, static_cast<std::uint32_t>(path.size() + 1));
        out.cstring(withBackslashes(path));
        out.chunkHeader(FC_MWID, 4);
        out.u32(0);

        ENTRY_MODF modf = parsed.modf;
        modf.nameID = 0; // indexes MWID/MWMO
        modf.flags &= ~MODF_FLAG_NAME_IS_FDID;
        out.chunkHeader(FC_MODF, sizeof(ENTRY_MODF));
        out.raw(&modf, sizeof(modf));
      }
    }

    return out.bytes;
  }

  // --------------------------------------------------------------------
  // WDL builders
  // --------------------------------------------------------------------

  std::vector<std::uint8_t> buildModernWdl(std::vector<std::uint8_t> const& legacy_wdl,
                                           ConversionFileSource const& source,
                                           FdidAllocator& fdids,
                                           std::vector<ConversionIssue>& issues)
  {
    std::vector<RawChunk> top;
    if (!walkChunks(legacy_wdl.data(), 0, legacy_wdl.size(), top, issues, "legacy WDL"))
      return {};

    std::array<WdlTileRecord, 4096> tiles{};
    if (!readWdlTiles(legacy_wdl, top, tiles, issues, "legacy WDL"))
      return {};

    // Legacy object chunks.
    std::vector<std::string> wmo_names;
    std::vector<ENTRY_MODF> placements;
    if (RawChunk const* mwmo = findChunk(top, FC_MWMO))
    {
      std::uint8_t const* blob = legacy_wdl.data() + mwmo->payload;
      std::size_t start = 0;
      for (std::size_t i = 0; i < mwmo->size; ++i)
      {
        if (blob[i] == 0)
        {
          wmo_names.emplace_back(reinterpret_cast<char const*>(blob + start), i - start);
          start = i + 1;
        }
      }
      if (mwmo->size > start)
        wmo_names.emplace_back(reinterpret_cast<char const*>(blob + start), mwmo->size - start);
    }
    if (RawChunk const* modf = findChunk(top, FC_MODF))
    {
      if (modf->size % sizeof(ENTRY_MODF) == 0)
      {
        placements.resize(modf->size / sizeof(ENTRY_MODF));
        if (!placements.empty())
          std::memcpy(placements.data(), legacy_wdl.data() + modf->payload, modf->size);
      }
      else
      {
        warn(issues, "legacy WDL MODF size is not a multiple of 64; placements dropped");
      }
    }

    ByteSink out;
    out.chunkHeader(FC_MVER, 4);
    out.u32(18);

    // MAOF placeholder, patched after the tile records are emitted.
    out.chunkHeader(FC_MAOF, 4096 * 4);
    std::size_t const maof_pos = out.position();
    std::vector<std::uint32_t> maof_entries(4096, 0);
    out.zeros(4096 * 4);

    emitWdlTiles(out, tiles, maof_entries);

    for (std::size_t i = 0; i < 4096; ++i)
      out.patchU32(maof_pos + i * 4, maof_entries[i]);

    if (!placements.empty())
    {
      ByteSink mlmd;
      ByteSink mlmx;
      for (std::size_t i = 0; i < placements.size(); ++i)
      {
        ENTRY_MODF const& modf = placements[i];
        if (modf.nameID >= wmo_names.size())
        {
          std::ostringstream message;
          message << "legacy WDL MODF entry " << i << " references WMO id " << modf.nameID
                  << " outside MWMO; entry dropped";
          warn(issues, message.str());
          continue;
        }
        std::string const& name = wmo_names[modf.nameID];
        if (name.empty())
        {
          warn(issues, "legacy WDL MODF entry references an empty WMO name; entry dropped");
          continue;
        }

        std::uint32_t const fdid = fdids.resolve(name, "wmo", source);

        MlmdRecord record{};
        record.nameID = fdid;
        record.uniqueID = modf.uniqueID;
        std::memcpy(record.pos, modf.pos, sizeof(record.pos));
        std::memcpy(record.rot, modf.rot, sizeof(record.rot));
        record.flags = static_cast<std::uint16_t>(modf.flags & ~MODF_FLAG_NAME_IS_FDID);
        record.doodadSet = modf.doodadSet;
        record.nameSet = modf.nameSet;
        record.scale = modf.scale;
        mlmd.raw(&record, sizeof(record));

        MlmxRecord bounds{};
        // MODF extents: [0] = upper, [1] = lower; MLMX: min/max/radius.
        bounds.min[0] = modf.extents[1].x;
        bounds.min[1] = modf.extents[1].y;
        bounds.min[2] = modf.extents[1].z;
        bounds.max[0] = modf.extents[0].x;
        bounds.max[1] = modf.extents[0].y;
        bounds.max[2] = modf.extents[0].z;
        float const dx = bounds.max[0] - bounds.min[0];
        float const dy = bounds.max[1] - bounds.min[1];
        float const dz = bounds.max[2] - bounds.min[2];
        bounds.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        mlmx.raw(&bounds, sizeof(bounds));
      }

      if (mlmd.bytes.size())
      {
        out.chunkHeader(FC_MLMD, static_cast<std::uint32_t>(mlmd.bytes.size()));
        out.raw(mlmd.bytes.data(), mlmd.bytes.size());
        out.chunkHeader(FC_MLMX, static_cast<std::uint32_t>(mlmx.bytes.size()));
        out.raw(mlmx.bytes.data(), mlmx.bytes.size());
      }
    }

    return out.bytes;
  }

  std::vector<std::uint8_t> buildLegacyWdl(std::vector<std::uint8_t> const& modern_wdl,
                                           ConversionFileSource const& source,
                                           FdidAllocator& fdids,
                                           std::vector<ConversionIssue>& issues)
  {
    (void)fdids;
    std::vector<RawChunk> top;
    if (!walkChunks(modern_wdl.data(), 0, modern_wdl.size(), top, issues, "modern WDL"))
      return {};

    std::array<WdlTileRecord, 4096> tiles{};
    if (!readWdlTiles(modern_wdl, top, tiles, issues, "modern WDL"))
      return {};

    RawChunk const* mlmd = findChunk(top, FC_MLMD);
    RawChunk const* mlmx = findChunk(top, FC_MLMX);
    if (findChunk(top, FC_MLDD) || findChunk(top, FC_MLDX))
      warn(issues, "modern WDL horizon doodads (MLDD/MLDX) have no legacy WDL slot; dropped");

    std::vector<MlmdRecord> records;
    if (mlmd && mlmd->size % sizeof(MlmdRecord) == 0)
    {
      records.resize(mlmd->size / sizeof(MlmdRecord));
      if (!records.empty())
        std::memcpy(records.data(), modern_wdl.data() + mlmd->payload, mlmd->size);
    }
    else if (mlmd)
    {
      warn(issues, "modern WDL MLMD size is not a multiple of 40; placements dropped");
    }
    std::vector<MlmxRecord> bounds;
    if (mlmx && mlmx->size % sizeof(MlmxRecord) == 0)
    {
      bounds.resize(mlmx->size / sizeof(MlmxRecord));
      if (!bounds.empty())
        std::memcpy(bounds.data(), modern_wdl.data() + mlmx->payload, mlmx->size);
    }
    if (mlmx && bounds.size() != records.size())
      warn(issues, "modern WDL MLMX count does not match MLMD; extents default to zero");

    ByteSink out;
    out.chunkHeader(FC_MVER, 4);
    out.u32(18);

    // MWMO/MWID/MODF (legacy order: MWMO -> MWID -> MODF -> MAOF -> tiles).
    if (!records.empty())
    {
      ByteSink mwmo;
      ByteSink mwid;
      ByteSink modf_sink;
      std::map<std::string, std::uint32_t> name_offsets;

      for (std::size_t i = 0; i < records.size(); ++i)
      {
        MlmdRecord const& record = records[i];
        std::string path;
        if (source.fdid_to_path)
          path = source.fdid_to_path(record.nameID);
        if (path.empty())
        {
          std::ostringstream message;
          message << "WDL placement FileDataID " << record.nameID
                  << " is not in the source listfile; emitted as a placeholder path";
          warn(issues, message.str());
          path = "missing\\fdid_" + std::to_string(record.nameID) + ".wmo";
        }
        path = withBackslashes(lower(path));

        auto it = name_offsets.find(path);
        std::uint32_t name_offset;
        if (it == name_offsets.end())
        {
          name_offset = static_cast<std::uint32_t>(mwmo.position());
          mwmo.cstring(path);
          name_offsets.emplace(path, name_offset);
        }
        else
        {
          name_offset = it->second;
        }

        ENTRY_MODF modf{};
        modf.nameID = static_cast<std::uint32_t>(mwid.position() / 4); // 1:1 MWID entries
        mwid.u32(name_offset);
        modf.uniqueID = record.uniqueID;
        std::memcpy(modf.pos, record.pos, sizeof(modf.pos));
        std::memcpy(modf.rot, record.rot, sizeof(modf.rot));
        if (i < bounds.size())
        {
          modf.extents[0] = glm::vec3(bounds[i].max[0], bounds[i].max[1], bounds[i].max[2]);
          modf.extents[1] = glm::vec3(bounds[i].min[0], bounds[i].min[1], bounds[i].min[2]);
        }
        modf.flags = static_cast<std::uint16_t>(record.flags & ~MODF_FLAG_NAME_IS_FDID);
        modf.doodadSet = record.doodadSet;
        modf.nameSet = record.nameSet;
        modf.scale = record.scale;
        modf_sink.raw(&modf, sizeof(modf));
      }

      out.chunkHeader(FC_MWMO, static_cast<std::uint32_t>(mwmo.bytes.size()));
      out.raw(mwmo.bytes.data(), mwmo.bytes.size());
      out.chunkHeader(FC_MWID, static_cast<std::uint32_t>(mwid.bytes.size()));
      out.raw(mwid.bytes.data(), mwid.bytes.size());
      out.chunkHeader(FC_MODF, static_cast<std::uint32_t>(modf_sink.bytes.size()));
      out.raw(modf_sink.bytes.data(), modf_sink.bytes.size());
    }

    out.chunkHeader(FC_MAOF, 4096 * 4);
    std::size_t const maof_pos = out.position();
    std::vector<std::uint32_t> maof_entries(4096, 0);
    out.zeros(4096 * 4);

    emitWdlTiles(out, tiles, maof_entries);

    for (std::size_t i = 0; i < 4096; ++i)
      out.patchU32(maof_pos + i * 4, maof_entries[i]);

    return out.bytes;
  }

  // --------------------------------------------------------------------
  // Orchestrator
  // --------------------------------------------------------------------

  ConversionReport convertMap(ConversionOptions const& options,
                              ConversionFileSource const& source)
  {
    ConversionReport report;
    report.target = options.target;
    report.map_name = options.map_name;
    report.map_id = options.map_id;
    report.target_build = options.target_build;

    auto add_warning = [&](std::string message)
    {
      report.warnings.push_back(std::move(message));
    };
    auto add_error = [&](std::string message)
    {
      report.errors.push_back(std::move(message));
    };
    auto take_issues = [&](std::vector<ConversionIssue> const& issues,
                           std::string const& prefix)
    {
      for (ConversionIssue const& issue : issues)
      {
        std::string message = prefix.empty() ? issue.message : prefix + ": " + issue.message;
        if (issue.severity == ConversionIssue::Severity::error)
          add_error(std::move(message));
        else
          add_warning(std::move(message));
      }
    };

    if (options.map_name.empty())
    {
      add_error("no map name given");
      return report;
    }
    if (!source.read_file)
    {
      add_error("conversion source has no file reader");
      return report;
    }

    std::string const name_lower = lower(options.map_name);
    std::string const base = "world/maps/" + name_lower + "/" + name_lower;
    std::string const out_base = "World/Maps/" + options.map_name + "/" + options.map_name;

    auto wdt_bytes = source.read_file(base + ".wdt");
    if (!wdt_bytes)
    {
      add_error("cannot read " + base + ".wdt");
      return report;
    }

    // Source format detection: MAID (or its MPHD flag) marks a modern WDT.
    ParsedWdt const probe = Noggit::Formats::WDT::parseWdt(*wdt_bytes, true);
    bool const source_is_modern =
      probe.maid_present
      || (probe.mphd_present && (probe.mphd.flags & MPHD_FLAG_HAS_MAID) != 0);
    report.source = source_is_modern ? MapFormat::modern : MapFormat::wotlk;

    if (report.source == options.target)
    {
      add_error(std::string("map is already in the target format (")
                + (source_is_modern ? "modern" : "WotLK") + ")");
      return report;
    }

    for (auto const& issue : probe.issues)
    {
      if (issue.severity == Noggit::Formats::WDT::IssueSeverity::error)
        add_warning("source WDT: " + issue.message);
    }

    FdidAllocator fdids(options.custom_fdid_base);
    bool const to_modern = options.target == MapFormat::modern;

    // The legacy WDT's MPHD big-alpha flag (0x4) decides how the source's
    // alpha planes are encoded; TBC-era WDTs without it use packed 4-bit
    // alpha.
    ConversionOptions effective = options;
    if (!source_is_modern)
      effective.big_alpha = (probe.mphd.flags & 0x4) != 0;

    //! fdid for one of the files the converter writes: prefer the client
    //! listfile id (reconverting an existing map), else allocate a custom
    //! one.
    auto file_fdid = [&](std::string const& output_relative_path,
                         char const* role) -> std::uint32_t
    {
      return fdids.resolve(output_relative_path, role, source);
    };

    std::array<MaidEntryOut, 4096> tile_fdids{};

    for (std::size_t i = 0; i < 4096; ++i)
    {
      if (!(probe.main_flags[i] & 1))
        continue;
      ++report.tiles_total;

      unsigned const x = static_cast<unsigned>(i % 64);
      unsigned const y = static_cast<unsigned>(i / 64);
      std::string const suffix = tileSuffix(x, y);
      std::string const tile_prefix = options.map_name + suffix;

      if (to_modern)
      {
        auto legacy = source.read_file(base + suffix + ".adt");
        if (!legacy)
        {
          ++report.tiles_skipped;
          add_warning(tile_prefix + ": legacy ADT not found; tile skipped");
          continue;
        }

        AdtConversionResult conv = convertAdtLegacyToModern(*legacy, effective, source, fdids);
        take_issues(conv.issues, tile_prefix);
        if (!conv.ok)
        {
          ++report.tiles_skipped;
          continue;
        }

        std::uint32_t const root_fdid =
          file_fdid(out_base + suffix + ".adt", "adt_root");
        std::uint32_t const tex0_fdid =
          file_fdid(out_base + suffix + "_tex0.adt", "adt_tex0");
        std::uint32_t const obj0_fdid =
          file_fdid(out_base + suffix + "_obj0.adt", "adt_obj0");

        tile_fdids[i].root_adt = root_fdid;
        tile_fdids[i].tex0 = tex0_fdid;
        tile_fdids[i].obj0 = obj0_fdid;

        report.files.push_back({out_base + suffix + ".adt", std::move(conv.root)});
        report.files.push_back({out_base + suffix + "_tex0.adt", std::move(conv.tex0)});
        report.files.push_back({out_base + suffix + "_obj0.adt", std::move(conv.obj0)});
        ++report.tiles_converted;
      }
      else
      {
        MaidEntry maid{};
        if (probe.maid_present && probe.maid.size() == 4096)
          maid = probe.maid[i];

        //! Modern tiles are located by MAID FileDataID; fall back to the
        //! canonical path names for hand-made/downported installs.
        auto read_part = [&](std::uint32_t fdid,
                             std::string const& canonical) -> std::vector<std::uint8_t>
        {
          if (fdid && source.fdid_to_path)
          {
            std::string const path = source.fdid_to_path(fdid);
            if (!path.empty())
            {
              if (auto bytes = source.read_file(normalize_path(path)))
                return *bytes;
            }
          }
          if (auto bytes = source.read_file(canonical))
            return *bytes;
          return {};
        };

        std::vector<std::uint8_t> const root = read_part(maid.root_adt, base + suffix + ".adt");
        std::vector<std::uint8_t> const tex0 = read_part(maid.tex0_adt, base + suffix + "_tex0.adt");
        std::vector<std::uint8_t> const obj0 = read_part(maid.obj0_adt, base + suffix + "_obj0.adt");
        std::vector<std::uint8_t> const obj1 = read_part(maid.obj1_adt, base + suffix + "_obj1.adt");

        if (root.empty() || tex0.empty())
        {
          ++report.tiles_skipped;
          add_warning(tile_prefix + ": modern ROOT/TEX0 parts not found; tile skipped");
          continue;
        }

        AdtConversionResult conv =
          convertAdtModernToLegacy(root, tex0, obj0, obj1, options, source, fdids);
        take_issues(conv.issues, tile_prefix);
        if (!conv.ok)
        {
          ++report.tiles_skipped;
          continue;
        }

        report.files.push_back({out_base + suffix + ".adt", std::move(conv.root)});
        ++report.tiles_converted;
      }
    }

    // ---- WDT ----
    std::vector<ConversionIssue> wdt_issues;
    std::vector<std::uint8_t> new_wdt;
    if (to_modern)
      new_wdt = buildModernWdt(*wdt_bytes, tile_fdids, source, fdids, wdt_issues);
    else
      new_wdt = buildLegacyWdt(*wdt_bytes, options, source, fdids, wdt_issues);
    take_issues(wdt_issues, "WDT");
    if (new_wdt.empty())
    {
      add_error("WDT conversion failed");
    }
    else
    {
      // buildModernWdt produces the Shadowlands family's WDT; a conversion
      // aimed at another modern generation steps from there, stripping what
      // the target does not carry and reporting what it carries but cannot be
      // had without fabricating content.
      if (to_modern && options.target_generation != MapGeneration::shadowlands_9_2_7)
      {
        auto step = convertWdtBetweenGenerations(
          new_wdt, MapGeneration::shadowlands_9_2_7, options.target_generation);
        for (auto const& note : step.notes)
          if (step.ok) add_warning(note); else add_error(note);
        if (step.ok)
          new_wdt = std::move(step.bytes);
      }
      report.files.push_back({out_base + ".wdt", std::move(new_wdt)});
    }

    // ---- WDL (optional) ----
    if (options.include_wdl)
    {
      if (auto wdl_bytes = source.read_file(base + ".wdl"))
      {
        std::vector<ConversionIssue> wdl_issues;
        std::vector<std::uint8_t> new_wdl;
        if (to_modern)
          new_wdl = buildModernWdl(*wdl_bytes, source, fdids, wdl_issues);
        else
          new_wdl = buildLegacyWdl(*wdl_bytes, source, fdids, wdl_issues);
        take_issues(wdl_issues, "WDL");
        if (!new_wdl.empty())
        {
          if (to_modern && options.target_generation != MapGeneration::shadowlands_9_2_7)
          {
            auto step = convertWdlBetweenGenerations(
              new_wdl, MapGeneration::shadowlands_9_2_7, options.target_generation);
            for (auto const& note : step.notes)
              if (step.ok) add_warning(note); else add_error(note);
            if (step.ok)
              new_wdl = std::move(step.bytes);
          }
          report.files.push_back({out_base + ".wdl", std::move(new_wdl)});
        }
      }
    }

    // ---- FileDataIDs for the map files themselves + registration ----
    if (to_modern)
    {
      std::uint32_t const wdt_fdid = file_fdid(out_base + ".wdt", "wdt");
      if (options.include_wdl)
        file_fdid(out_base + ".wdl", "wdl");

      if (options.map_id > 0)
      {
        // Same field set the map wizard persists for
        // extraData/modernMapRegistrations.json (validated by
        // ModernMapRegistrations::validate); values are conservative
        // defaults the user should review before merging.
        std::string directory;
        for (char c : options.map_name)
          directory += (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            ? c : '_';

        report.suggested_registration_fields = {
          {"Directory", directory},
          {"InstanceType", "0"},
          {"Flags", "0"},
          {"MapName_lang", options.map_name},
          {"AreaTableID", "0"},
          {"ExpansionID", "8"}, // Shadowlands; adjust for other targets
          {"MaxPlayers", "0"},
          {"LoadingScreenID", "0"},
          {"MinimapIconScale", "1"},
          {"WdtFileDataID", std::to_string(wdt_fdid)},
        };
      }
    }

    report.fdid_assignments = fdids.assignments();

    if (report.tiles_total == 0)
      add_warning("the source WDT marks no tiles as having ADT data;"
                  " only map-level files were converted");

    report.ok = report.errors.empty();
    return report;
  }

  // --------------------------------------------------------------------
  // Manifest
  // --------------------------------------------------------------------

  // ---- generation conversion ----

  namespace
  {
    constexpr std::uint32_t gen_cc(char a, char b, char c, char d) noexcept
    {
      return (std::uint32_t(static_cast<unsigned char>(a)) << 24)
           | (std::uint32_t(static_cast<unsigned char>(b)) << 16)
           | (std::uint32_t(static_cast<unsigned char>(c)) << 8)
           | std::uint32_t(static_cast<unsigned char>(d));
    }

    struct GenChunk
    {
      std::uint32_t magic;
      std::size_t offset;
      std::size_t size;
    };

    //! Walk a file's top-level chunks, requiring the walk to end exactly at the
    //! end of the file: a generation step copies bytes verbatim, so a file it
    //! cannot account for completely is a file it must not touch.
    bool gen_walk(std::vector<std::uint8_t> const& bytes,
                  std::vector<GenChunk>& out, std::string* error)
    {
      std::size_t pos = 0;
      while (pos + 8 <= bytes.size())
      {
        GenChunk chunk{};
        std::memcpy(&chunk.magic, bytes.data() + pos, 4);
        std::memcpy(&chunk.size, bytes.data() + pos + 4, 4);
        chunk.offset = pos;
        if (chunk.size > bytes.size() - pos - 8)
        {
          if (error)
            *error = "truncated chunk at offset " + std::to_string(pos);
          return false;
        }
        out.push_back(chunk);
        pos += 8 + chunk.size;
      }
      if (pos != bytes.size())
      {
        if (error)
          *error = "the walk ends " + std::to_string(bytes.size() - pos)
                 + " byte(s) inside a chunk tag";
        return false;
      }
      return true;
    }

    std::string gen_chunk_text(std::uint32_t magic)
    {
      std::string text;
      for (int shift = 24; shift >= 0; shift -= 8)
      {
        auto const character = static_cast<char>((magic >> shift) & 0xFF);
        text += (character >= 32 && character < 127) ? character : '?';
      }
      return text;
    }

    bool gen_carries_wdt_chunk(std::uint32_t magic, GenerationFeatures const& target)
    {
      if (magic == gen_cc('M', 'A', 'I', '2'))
        return target.wdt_mai2;
      return true; // every other chunk a modern WDT carries, both families carry
    }

    bool gen_carries_wdl_chunk(std::uint32_t magic, GenerationFeatures const& target)
    {
      if (magic == gen_cc('M', 'S', 'S', 'F'))
        return target.wdl_mssf;
      return true;
    }

    bool gen_has(std::vector<GenChunk> const& chunks, std::uint32_t magic)
    {
      for (auto const& chunk : chunks)
        if (chunk.magic == magic)
          return true;
      return false;
    }

    GenerationConversionReport gen_convert_file(std::vector<std::uint8_t> const& source,
                                                MapGeneration from, MapGeneration to,
                                                bool is_wdt, char const* what)
    {
      GenerationConversionReport report;

      if (from == MapGeneration::wotlk_3_3_5 || to == MapGeneration::wotlk_3_3_5)
      {
        report.notes.push_back(std::string(what)
          + ": WotLK is a different conversion (convertMap), not a step between"
            " modern generations.");
        return report;
      }

      if (from == to)
      {
        report.bytes = source;
        report.ok = true;
        return report;
      }

      auto const& target = generationFeatures(to);
      std::vector<GenChunk> chunks;
      std::string error;
      if (!gen_walk(source, chunks, &error))
      {
        report.notes.push_back(std::string(what) + ": the source does not walk, "
                               + error + "; leaving it untouched.");
        return report;
      }

      report.bytes.reserve(source.size());
      for (auto const& chunk : chunks)
      {
        bool const carried = is_wdt ? gen_carries_wdt_chunk(chunk.magic, target)
                                    : gen_carries_wdl_chunk(chunk.magic, target);
        if (!carried)
        {
          report.notes.push_back(std::string(what) + ": stripped "
                                 + gen_chunk_text(chunk.magic) + ", which the "
                                 + target.label + " family does not carry.");
          continue;
        }
        report.bytes.insert(report.bytes.end(),
                            source.begin() + static_cast<std::ptrdiff_t>(chunk.offset),
                            source.begin() + static_cast<std::ptrdiff_t>(chunk.offset + 8 + chunk.size));
      }

      // Chunks the target family carries but the source lacks are reported,
      // never invented: MAI2 is a per-tile FileDataID table and MSSF a sky-node
      // table, and fabricating either offline would write ids and nodes the
      // target client cannot resolve. The right source for them is the target
      // client's own copy of the file, which is what the save path already does
      // for stale overrides.
      if (is_wdt && target.wdt_mai2 && !gen_has(chunks, gen_cc('M', 'A', 'I', '2')))
        report.notes.push_back(std::string(what) + ": the " + target.label
          + " family carries MAI2 but the source has none; it stays absent,"
            " because authoring per-tile FileDataIDs offline would fabricate ids"
            " the target client cannot resolve. Take it from the target client's"
            " own WDT where one is available.");
      if (!is_wdt && target.wdl_mssf && !gen_has(chunks, gen_cc('M', 'S', 'S', 'F')))
        report.notes.push_back(std::string(what) + ": the " + target.label
          + " family carries MSSF but the source has none; it stays absent rather"
            " than fabricated.");

      report.ok = true;
      return report;
    }
  }

  GenerationFeatures const& generationFeatures(MapGeneration generation)
  {
    static GenerationFeatures const wotlk = {
      MapGeneration::wotlk_3_3_5, "3.3.5", false, false, false, false};
    static GenerationFeatures const shadowlands = {
      MapGeneration::shadowlands_9_2_7, "9.2.7", true, false, false, true};
    static GenerationFeatures const retail = {
      MapGeneration::retail_12_x, "12.x", true, true, true, true};

    switch (generation)
    {
    case MapGeneration::wotlk_3_3_5: return wotlk;
    case MapGeneration::retail_12_x: return retail;
    case MapGeneration::shadowlands_9_2_7: break;
    }
    return shadowlands;
  }

  MapGeneration detectModernGeneration(std::vector<std::uint8_t> const& wdt,
                                      std::vector<std::uint8_t> const& wdl)
  {
    std::vector<GenChunk> chunks;
    std::string error;

    if (gen_walk(wdt, chunks, &error))
      if (gen_has(chunks, gen_cc('M', 'A', 'I', '2')))
        return MapGeneration::retail_12_x;

    chunks.clear();
    if (gen_walk(wdl, chunks, &error))
      if (gen_has(chunks, gen_cc('M', 'S', 'S', 'F')))
        return MapGeneration::retail_12_x;

    return MapGeneration::shadowlands_9_2_7;
  }

  GenerationConversionReport convertWdtBetweenGenerations(
      std::vector<std::uint8_t> const& source, MapGeneration from, MapGeneration to)
  {
    return gen_convert_file(source, from, to, true, "WDT");
  }

  GenerationConversionReport convertWdlBetweenGenerations(
      std::vector<std::uint8_t> const& source, MapGeneration from, MapGeneration to)
  {
    return gen_convert_file(source, from, to, false, "WDL");
  }

  GenerationConversionReport convertAdtPartBetweenGenerations(
      std::vector<std::uint8_t> const& source, MapGeneration from, MapGeneration to)
  {
    GenerationConversionReport report;

    if (from == MapGeneration::wotlk_3_3_5 || to == MapGeneration::wotlk_3_3_5)
    {
      report.notes.push_back("ADT part: WotLK is a different conversion"
                             " (convertMap), not a step between modern generations.");
      return report;
    }

    std::vector<GenChunk> chunks;
    std::string error;
    if (!gen_walk(source, chunks, &error))
    {
      report.notes.push_back("ADT part: the source does not walk, " + error + ".");
      return report;
    }

    bool version_ok = false;
    for (auto const& chunk : chunks)
    {
      if (chunk.magic != gen_cc('M', 'V', 'E', 'R'))
        continue;
      if (chunk.size < 4)
        break;
      std::uint32_t version = 0;
      std::memcpy(&version, source.data() + chunk.offset + 8, 4);
      version_ok = (version == 18);
      break;
    }

    if (!version_ok)
    {
      report.notes.push_back("ADT part: no MVER 18 chunk; both modern families"
                             " use version 18, so this is not a modern part.");
      return report;
    }

    // The ADT layout is identical across the modern families: same version,
    // same chunk set, same offsets. Conversion between them is the statement
    // that nothing changes, checked.
    if (from != to)
      report.notes.push_back(std::string("ADT part: the ")
        + generationFeatures(from).label + " and " + generationFeatures(to).label
        + " families share the ADT layout (MVER 18); copied unchanged after the"
          " part walked clean.");

    report.bytes = source;
    report.ok = true;
    return report;
  }

  std::string ConversionReport::manifest_json() const
  {
    std::ostringstream out;
    out << "{\n";
    out << "  \"format_version\": 1,\n";
    out << "  \"source_format\": \"" << (source == MapFormat::modern ? "modern" : "wotlk") << "\",\n";
    out << "  \"target_format\": \"" << (target == MapFormat::modern ? "modern" : "wotlk") << "\",\n";
    out << "  \"map_name\": \"" << jsonEscape(map_name) << "\",\n";
    out << "  \"map_id\": " << map_id << ",\n";
    out << "  \"target_build\": \"" << jsonEscape(target_build) << "\",\n";
    out << "  \"stats\": {\n";
    out << "    \"tiles_total\": " << tiles_total << ",\n";
    out << "    \"tiles_converted\": " << tiles_converted << ",\n";
    out << "    \"tiles_skipped\": " << tiles_skipped << "\n";
    out << "  },\n";

    out << "  \"files\": [\n";
    for (std::size_t i = 0; i < files.size(); ++i)
    {
      out << "    {\"path\": \"" << jsonEscape(files[i].relative_path)
          << "\", \"size\": " << files[i].bytes.size() << "}"
          << (i + 1 < files.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"fdid_assignments\": [\n";
    for (std::size_t i = 0; i < fdid_assignments.size(); ++i)
    {
      FdidAssignment const& a = fdid_assignments[i];
      out << "    {\"fdid\": " << a.fdid
          << ", \"path\": \"" << jsonEscape(a.path)
          << "\", \"role\": \"" << jsonEscape(a.role)
          << "\", \"existed_in_client\": " << (a.existed_in_client ? "true" : "false") << "}"
          << (i + 1 < fdid_assignments.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    if (!suggested_registration_fields.empty())
    {
      out << "  \"registration\": {\n";
      out << "    \"id\": " << map_id << ",\n";
      out << "    \"target_build\": \"" << jsonEscape(target_build) << "\",\n";
      out << "    \"fields\": {\n";
      std::size_t n = 0;
      for (auto const& field : suggested_registration_fields)
      {
        out << "      \"" << jsonEscape(field.first) << "\": \""
            << jsonEscape(field.second) << "\""
            << (++n < suggested_registration_fields.size() ? "," : "") << "\n";
      }
      out << "    },\n";
      out << "    \"array_fields\": {\n";
      n = 0;
      for (auto const& field : suggested_registration_array_fields)
      {
        out << "      \"" << jsonEscape(field.first) << "\": [";
        for (std::size_t k = 0; k < field.second.size(); ++k)
          out << "\"" << jsonEscape(field.second[k]) << "\""
              << (k + 1 < field.second.size() ? ", " : "");
        out << "]" << (++n < suggested_registration_array_fields.size() ? "," : "") << "\n";
      }
      out << "    }\n";
      out << "  },\n";
    }

    out << "  \"warnings\": [\n";
    for (std::size_t i = 0; i < warnings.size(); ++i)
      out << "    \"" << jsonEscape(warnings[i]) << "\""
          << (i + 1 < warnings.size() ? "," : "") << "\n";
    out << "  ],\n";
    out << "  \"errors\": [\n";
    for (std::size_t i = 0; i < errors.size(); ++i)
      out << "    \"" << jsonEscape(errors[i]) << "\""
          << (i + 1 < errors.size() ? "," : "") << "\n";
    out << "  ]\n";
    out << "}\n";
    return out.str();
  }
}
