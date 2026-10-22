// Two-way WotLK <-> modern ADT conversion (see docs/MAP_CONVERSION.md).
//
// Qt-free, logging-free: everything the converter cannot decide on its own
// (FileDataID resolution) comes in through ConversionFileSource callbacks,
// so this translation unit builds and runs in the standalone harness.
//
// Layout facts implemented here are the ones verified by the fork's other
// format modules:
//  * WotLK (3.3.5) and Shadowlands (9.2.7) share the v18 128-byte MCNK
//    header and byte-identical MCVT/MCNR/MCSH/MCSE payloads - the split
//    format moves MCLY/MCAL into _tex0.adt and MDDF/MODF into _obj0.adt
//    and switches name references to FileDataIDs (ModernAdtReader/Writer,
//    ShadowlandsADTCodec).
//  * Legacy MCLY.textureID is a byte offset into MTEX; modern MCLY.
//    textureID is an index into MDID (ShadowlandsADTCodec validation).
//  * MHDR offsets are stored relative to the MHDR payload start (the
//    classic "- 0x14" convention of MapTile::save); readers add 0x14.
//    The basis is self-calibrated from the source file like
//    ModernAdtWriter::mhdr_offset_basis does.
//  * Legacy MCIN entries: offset = absolute MCNK chunk start, size =
//    payload + 8 (MapTile::save / MapChunk::save).
//  * WotLK compressed alpha (MCLY flag 0x200) uses the exact RLE scheme of
//    modern MCAL (7-bit count, bit 7 = fill; Alphamap::readCompressed ==
//    decodeModernAlpha), so one codec serves both families.
//  * MH2O exists since WotLK and is structurally identical in both
//    families: it passes through untouched. Per-chunk MCLQ (pre-WotLK /
//    export format) is converted to MH2O on the way to modern using the
//    same record layout Noggit's own MH2O writer emits (ChunkWater::save /
//    liquid_layer::save).

#include <noggit/formats/conversion/MapConversion.hpp>

#include <noggit/MapHeaders.h>
#include <noggit/formats/adt/ModernAdtReader.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <sstream>

namespace Noggit::Formats::Conversion
{
  //! Named (not anonymous) so unity builds cannot merge these helpers
  //! with the identically shaped ones in MapConversion.cpp.
  namespace adt_detail
  {
    constexpr std::uint32_t FC_MVER = (std::uint32_t('M') << 24) | (std::uint32_t('V') << 16) | (std::uint32_t('E') << 8) | std::uint32_t('R');
    constexpr std::uint32_t FC_MHDR = (std::uint32_t('M') << 24) | (std::uint32_t('H') << 16) | (std::uint32_t('D') << 8) | std::uint32_t('R');
    constexpr std::uint32_t FC_MCIN = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('I') << 8) | std::uint32_t('N');
    constexpr std::uint32_t FC_MTEX = (std::uint32_t('M') << 24) | (std::uint32_t('T') << 16) | (std::uint32_t('E') << 8) | std::uint32_t('X');
    constexpr std::uint32_t FC_MMDX = (std::uint32_t('M') << 24) | (std::uint32_t('M') << 16) | (std::uint32_t('D') << 8) | std::uint32_t('X');
    constexpr std::uint32_t FC_MMID = (std::uint32_t('M') << 24) | (std::uint32_t('M') << 16) | (std::uint32_t('I') << 8) | std::uint32_t('D');
    constexpr std::uint32_t FC_MWMO = (std::uint32_t('M') << 24) | (std::uint32_t('W') << 16) | (std::uint32_t('M') << 8) | std::uint32_t('O');
    constexpr std::uint32_t FC_MWID = (std::uint32_t('M') << 24) | (std::uint32_t('W') << 16) | (std::uint32_t('I') << 8) | std::uint32_t('D');
    constexpr std::uint32_t FC_MDDF = (std::uint32_t('M') << 24) | (std::uint32_t('D') << 16) | (std::uint32_t('D') << 8) | std::uint32_t('F');
    constexpr std::uint32_t FC_MODF = (std::uint32_t('M') << 24) | (std::uint32_t('O') << 16) | (std::uint32_t('D') << 8) | std::uint32_t('F');
    constexpr std::uint32_t FC_MFBO = (std::uint32_t('M') << 24) | (std::uint32_t('F') << 16) | (std::uint32_t('B') << 8) | std::uint32_t('O');
    constexpr std::uint32_t FC_MH2O = (std::uint32_t('M') << 24) | (std::uint32_t('H') << 16) | (std::uint32_t('2') << 8) | std::uint32_t('O');

    //! The MCNK flag bits that claim per-chunk liquid (river, ocean, magma,
    //! slime). In the split format a modern client answers "does this chunk have
    //! liquid" from the tile's MH2O, not from MCLQ, so these bits are only
    //! meaningful together with an MH2O chunk.
    constexpr std::uint32_t legacy_liquid_flags = 0x4u | 0x8u | 0x10u | 0x20u;
    constexpr std::uint32_t FC_MCNK = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('N') << 8) | std::uint32_t('K');
    constexpr std::uint32_t FC_MCVT = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('V') << 8) | std::uint32_t('T');
    constexpr std::uint32_t FC_MCNR = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('N') << 8) | std::uint32_t('R');
    constexpr std::uint32_t FC_MCSH = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('S') << 8) | std::uint32_t('H');
    constexpr std::uint32_t FC_MCAL = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('A') << 8) | std::uint32_t('L');
    constexpr std::uint32_t FC_MCLY = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('L') << 8) | std::uint32_t('Y');
    constexpr std::uint32_t FC_MCLQ = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('L') << 8) | std::uint32_t('Q');
    constexpr std::uint32_t FC_MCRF = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('R') << 8) | std::uint32_t('F');
    constexpr std::uint32_t FC_MCRD = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('R') << 8) | std::uint32_t('D');
    constexpr std::uint32_t FC_MCRW = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('R') << 8) | std::uint32_t('W');
    constexpr std::uint32_t FC_MCSE = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('S') << 8) | std::uint32_t('E');
    constexpr std::uint32_t FC_MCCV = (std::uint32_t('M') << 24) | (std::uint32_t('C') << 16) | (std::uint32_t('C') << 8) | std::uint32_t('V');
    constexpr std::uint32_t FC_MDID = (std::uint32_t('M') << 24) | (std::uint32_t('D') << 16) | (std::uint32_t('I') << 8) | std::uint32_t('D');

    constexpr std::uint32_t MCLY_FLAG_USE_ALPHA = 0x100;
    constexpr std::uint32_t MCLY_FLAG_ALPHA_COMPRESSED = 0x200;

    constexpr std::uint32_t MODF_FLAG_NAME_IS_FDID = 0x8;

    //! Matches MAX_TEXTURE_LAYERS (Alphamap.hpp) - the per-chunk layer cap
    //! of both families.
    constexpr std::size_t MAX_CHUNK_LAYERS = 16;



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
    std::uint16_t readU16(std::uint8_t const* p)
    {
      return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
    }

    //! One chunk of a RIFF-style walk: absolute payload offset and size.
    struct RawChunk
    {
      std::uint32_t magic = 0;
      std::uint32_t size = 0;
      std::size_t payload = 0;
      std::size_t start() const { return payload - 8; }
    };

    //! Walk [from, to) as a sequence of chunks.
    //! \param skip_mcnr_padding skip the 13 raw bytes Noggit's canonical
    //!        serializer emits after MCNR ("unknown MCNR bytes ... just to make
    //!        the files more blizzlike"). They are padding, not a sub-chunk
    //!        header: walking them as one consumes 8 of the 13 and leaves the
    //!        next read starting 5 bytes early, inside the padding, so the magic
    //!        comes out zero and the size picks up the leading bytes of the next
    //!        chunk - which reports a truncated chunk and fails the whole tile.
    //! \param require_exact also fail when the walk stops short of \a to, rather
    //!        than accepting whatever it managed to read.
    //!        A size-0 MCLQ is repaired from the MCNK header when \a
    //!        skip_mcnr_padding marks the walk as a legacy MCNK walk.
    //! On failure an issue is appended when \a issues is given, and \a failure
    //! receives the description.
    bool walkChunksCore(std::uint8_t const* data,
                        std::size_t from,
                        std::size_t to,
                        std::vector<RawChunk>& out,
                        std::vector<ConversionIssue>* issues,
                        char const* context,
                        bool skip_mcnr_padding,
                        bool require_exact,
                        std::string* failure)
    {
      auto const fail = [&](std::string message)
      {
        if (issues)
          issues->push_back({ConversionIssue::Severity::error, context + message});
        if (failure)
          *failure = message;
        return false;
      };

      // Legacy MCNK walks start right after the 128 byte MapChunkHeader, so the
      // header is readable back from `from` and can vouch for a size-0 MCLQ.
      std::size_t const mcnk_header =
        skip_mcnr_padding && from >= sizeof(MapChunkHeader)
          ? from - sizeof(MapChunkHeader)
          : static_cast<std::size_t>(-1);

      std::size_t pos = from;
      while (pos + 8 <= to)
      {
        RawChunk chunk;
        chunk.magic = readMagic(data + pos);
        chunk.size = readU32(data + pos + 4);
        chunk.payload = pos + 8;
        if (chunk.magic == FC_MCLQ && chunk.size == 0 && mcnk_header != static_cast<std::size_t>(-1))
        {
          std::uint32_t ofs_liquid = 0, size_liquid = 0;
          std::memcpy(&ofs_liquid, data + mcnk_header + offsetof(MapChunkHeader, ofsLiquid), 4);
          std::memcpy(&size_liquid, data + mcnk_header + offsetof(MapChunkHeader, sizeLiquid), 4);
          if (ofs_liquid == pos - mcnk_header + 8 && size_liquid > 8
              && size_liquid - 8 <= to - chunk.payload)
            chunk.size = size_liquid - 8;
        }
        if (chunk.size > to - chunk.payload)
        {
          std::ostringstream message;
          message << ": truncated chunk at offset " << pos
                  << " (declared size " << chunk.size << ")";
          return fail(message.str());
        }
        out.push_back(chunk);
        pos = chunk.payload + chunk.size;

        if (skip_mcnr_padding && chunk.magic == FC_MCNR && pos + 13 <= to)
          pos += 13;
      }
      if (require_exact && pos != to)
      {
        std::ostringstream message;
        message << ": walk stopped at offset " << pos << ", " << to
                << " expected (does not end on a chunk boundary)";
        return fail(message.str());
      }
      return true;
    }

    bool walkChunks(std::uint8_t const* data,
                    std::size_t from,
                    std::size_t to,
                    std::vector<RawChunk>& out,
                    std::vector<ConversionIssue>& issues,
                    char const* context,
                    bool legacy_mcnr_padding = false)
    {
      if (!legacy_mcnr_padding)
        return walkChunksCore(data, from, to, out, &issues, context, false, false, nullptr);

      // Retail MCNKs put the next sub-chunk's header straight after MCNR,
      // Noggit's put 13 padding bytes there first, and the two cannot be told
      // apart by inspecting those bytes: the padding is only recognisable while
      // it is zero filled, and nothing guarantees a serializer leaves zeros in a
      // gap it merely reserved. Pick by which convention consumes the payload
      // exactly - at most one can, since skipping in a retail MCNK lands
      // mid-chunk and not skipping in a Noggit MCNK desynchronizes the walk.
      std::vector<RawChunk> attempt;
      std::string without_padding;
      if (walkChunksCore(data, from, to, attempt, nullptr, context, false, true, &without_padding))
      {
        out = std::move(attempt);
        return true;
      }
      attempt.clear();
      std::string with_padding;
      if (walkChunksCore(data, from, to, attempt, nullptr, context, true, true, &with_padding))
      {
        out = std::move(attempt);
        return true;
      }

      // Neither convention accounts for the payload. Report the permissive walk
      // as before rather than the stricter one, so a file that only ever
      // partially parsed keeps behaving the way it did.
      return walkChunksCore(data, from, to, out, &issues, context, false, false, nullptr);
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
      void u16(std::uint16_t v)
      {
        bytes.push_back(static_cast<std::uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
      }
      void u8(std::uint8_t v)
      {
        bytes.push_back(v);
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
      void zeros(std::size_t size)
      {
        bytes.insert(bytes.end(), size, 0);
      }
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
      void chunkFromString(std::uint32_t magic, std::vector<std::uint8_t> const& payload)
      {
        chunkFrom(magic, payload.data(), payload.size());
      }
    };

    void warn(std::vector<ConversionIssue>& issues, std::string message)
    {
      issues.push_back({ConversionIssue::Severity::warning, std::move(message)});
    }

    void fail(std::vector<ConversionIssue>& issues, std::string message)
    {
      issues.push_back({ConversionIssue::Severity::error, std::move(message)});
    }

    // ------------------------------------------------------------------
    // Alpha codecs. The WotLK compressed scheme and modern MCAL are the
    // same RLE (7-bit count, bit 7 = fill), so decoding is shared; the
    // encoders differ per target family.
    // ------------------------------------------------------------------

    bool decodeAlphaPlane(std::uint8_t const* data,
                          std::size_t size,
                          std::uint32_t mcly_flags,
                          bool big_alpha,
                          std::vector<std::uint8_t>& out,
                          std::vector<ConversionIssue>& issues,
                          char const* context)
    {
      out.assign(4096, 0);

      if (mcly_flags & MCLY_FLAG_ALPHA_COMPRESSED)
      {
        std::size_t in = 0;
        std::size_t filled = 0;
        while (in < size && filled < 4096)
        {
          std::uint8_t const header_byte = data[in++];
          std::size_t const count = header_byte & 0x7F;
          bool const is_fill = (header_byte & 0x80) != 0;
          if (count == 0)
            continue;
          std::size_t const write_count = std::min(count, 4096 - filled);
          if (is_fill)
          {
            if (in >= size)
            {
              warn(issues, std::string(context) + ": truncated fill entry in compressed alpha");
              break;
            }
            std::memset(out.data() + filled, data[in], write_count);
            ++in;
          }
          else
          {
            if (in + write_count > size)
            {
              warn(issues, std::string(context) + ": truncated copy entry in compressed alpha");
              break;
            }
            std::memcpy(out.data() + filled, data + in, write_count);
            in += write_count;
          }
          filled += write_count;
        }
        return true;
      }

      if (big_alpha || size >= 4096)
      {
        if (size < 4096)
        {
          std::ostringstream message;
          message << context << ": uncompressed alpha is " << size
                  << " bytes, expected 4096";
          fail(issues, message.str());
          return false;
        }
        std::memcpy(out.data(), data, 4096);
        return true;
      }

      if (size >= 2048)
      {
        // TBC-era packed 4-bit alpha (Alphamap::readNotCompressed without
        // the edge fix-up: the converter keeps the raw values).
        for (std::size_t i = 0; i < 2048; ++i)
        {
          std::uint8_t const lower = data[i] & 0x0F;
          std::uint8_t const upper = (data[i] >> 4) & 0x0F;
          out[i * 2 + 0] = static_cast<std::uint8_t>(lower | (lower << 4));
          out[i * 2 + 1] = static_cast<std::uint8_t>(upper | (upper << 4));
        }
        return true;
      }

      std::ostringstream message;
      message << context << ": alpha data is " << size << " bytes; cannot decode";
      fail(issues, message.str());
      return false;
    }

    //! RLE-encode a 4096-byte alpha plane (fill runs of 4+ become fill
    //! entries, everything else copy entries). Returns empty when the
    //! result would not be smaller than the raw plane.
    std::vector<std::uint8_t> encodeAlphaRle(std::vector<std::uint8_t> const& plane)
    {
      std::vector<std::uint8_t> out;
      out.reserve(512);
      std::vector<std::uint8_t> copy_buffer;

      auto flush_copy = [&]
      {
        std::size_t index = 0;
        while (index < copy_buffer.size())
        {
          std::size_t const count = std::min<std::size_t>(127, copy_buffer.size() - index);
          out.push_back(static_cast<std::uint8_t>(count)); // mode bit 0 = copy
          out.insert(out.end(), copy_buffer.begin() + index, copy_buffer.begin() + index + count);
          index += count;
        }
        copy_buffer.clear();
      };

      std::size_t pos = 0;
      while (pos < plane.size())
      {
        std::size_t run = 1;
        while (pos + run < plane.size() && run < 127 && plane[pos + run] == plane[pos])
          ++run;

        if (run >= 4)
        {
          flush_copy();
          out.push_back(static_cast<std::uint8_t>(run | 0x80)); // fill
          out.push_back(plane[pos]);
          pos += run;
        }
        else
        {
          copy_buffer.push_back(plane[pos]);
          if (copy_buffer.size() == 127)
            flush_copy();
          ++pos;
        }
      }
      flush_copy();

      if (out.size() >= 4096)
        return {};
      return out;
    }

    //! Pack a 4096-byte plane into the TBC-era 2048-byte 4-bit form.
    std::vector<std::uint8_t> packSmallAlpha(std::vector<std::uint8_t> const& plane)
    {
      std::vector<std::uint8_t> out(2048);
      for (std::size_t i = 0; i < 2048; ++i)
        out[i] = static_cast<std::uint8_t>((plane[i * 2] >> 4) | ((plane[i * 2 + 1] >> 4) << 4));
      return out;
    }

    // ------------------------------------------------------------------
    // Legacy (monolithic WotLK) parse model
    // ------------------------------------------------------------------

    struct LegacyChunkModel
    {
      bool present = false;
      std::uint32_t mcin_flags = 0;
      std::uint32_t mcin_async_id = 0;
      MapChunkHeader header{};
      std::vector<ENTRY_MCLY> layers;
      RawChunk mcvt{};  bool has_mcvt = false;
      RawChunk mcnr{};  bool has_mcnr = false;
      RawChunk mcsh{};  bool has_mcsh = false;
      RawChunk mcse{};  bool has_mcse = false;
      RawChunk mccv{};  bool has_mccv = false;
      RawChunk mcal{};  bool has_mcal = false;
      RawChunk mclq{};  bool has_mclq = false;
      RawChunk mcrf{};  bool has_mcrf = false;
      bool has_refs = false; // MCRF seen
    };

    struct LegacyAdtModel
    {
      bool ok = false;
      std::uint32_t mhdr_flags = 0;
      std::uint32_t mhdr_offset_basis = 0x14;
      std::vector<std::string> textures;                      // MTEX order
      std::map<std::uint32_t, std::size_t> texture_offsets;   // MTEX offset -> index
      std::vector<std::string> m2_names;                      // via MMID/MMDX
      std::vector<std::string> wmo_names;                     // via MWID/MWMO
      std::vector<ENTRY_MDDF> mddf;                           // nameID indexes m2_names
      std::vector<ENTRY_MODF> modf;                           // nameID indexes wmo_names
      bool has_mfbo = false;  RawChunk mfbo{};
      bool has_mh2o = false;  RawChunk mh2o{};
      bool has_mtxf = false;
      std::array<LegacyChunkModel, 256> chunks{};
    };

    std::vector<std::string> parseStringTable(std::uint8_t const* data, std::size_t size)
    {
      std::vector<std::string> result;
      std::size_t start = 0;
      for (std::size_t i = 0; i < size; ++i)
      {
        if (data[i] == 0)
        {
          if (i > start)
            result.emplace_back(reinterpret_cast<char const*>(data + start), i - start);
          start = i + 1;
        }
      }
      if (size > start)
        result.emplace_back(reinterpret_cast<char const*>(data + start), size - start);
      return result;
    }

    //! Resolve an offset table (MMID/MWID) against a name blob
    //! (MMDX/MWMO): result[i] = string at names_blob + offsets[i].
    std::vector<std::string> resolveOffsetNames(std::uint8_t const* names_blob,
                                                std::size_t names_size,
                                                std::uint8_t const* offsets_blob,
                                                std::size_t offsets_size,
                                                std::vector<ConversionIssue>& issues,
                                                char const* context)
    {
      std::vector<std::string> result;
      std::size_t const count = offsets_size / 4;
      result.reserve(count);
      for (std::size_t i = 0; i < count; ++i)
      {
        std::uint32_t const offset = readU32(offsets_blob + i * 4);
        if (offset >= names_size)
        {
          std::ostringstream message;
          message << context << ": name offset " << offset << " is outside the name table";
          warn(issues, message.str());
          result.emplace_back();
          continue;
        }
        std::size_t end = offset;
        while (end < names_size && names_blob[end] != 0)
          ++end;
        result.emplace_back(reinterpret_cast<char const*>(names_blob + offset), end - offset);
      }
      return result;
    }

    bool parseLegacyAdt(std::vector<std::uint8_t> const& bytes,
                        LegacyAdtModel& model,
                        std::vector<ConversionIssue>& issues)
    {
      if (bytes.size() < 8 + 8 + 0x40)
      {
        fail(issues, "legacy ADT is too small");
        return false;
      }

      std::vector<RawChunk> top;
      if (!walkChunks(bytes.data(), 0, bytes.size(), top, issues, "legacy ADT"))
        return false;

      RawChunk const* mver = findChunk(top, FC_MVER);
      if (!mver || mver->size < 4 || readU32(bytes.data() + mver->payload) != 18)
      {
        fail(issues, "legacy ADT is not MVER 18 (only WotLK-family v18 files convert)");
        return false;
      }

      RawChunk const* mhdr = findChunk(top, FC_MHDR);
      if (!mhdr || mhdr->size < 0x40)
      {
        fail(issues, "legacy ADT has no usable MHDR");
        return false;
      }
      MHDR header{};
      std::memcpy(&header, bytes.data() + mhdr->payload, sizeof(header));
      model.mhdr_flags = header.flags;

      RawChunk const* mcin = findChunk(top, FC_MCIN);
      if (!mcin || mcin->size < 256 * sizeof(ENTRY_MCIN))
      {
        fail(issues, "legacy ADT has no usable MCIN");
        return false;
      }

      // Self-calibrate the MHDR offset basis: Noggit-written files store
      // "absolute - 0x14" (MHDR payload starts at 0x14 in their layout),
      // but the source file decides.
      model.mhdr_offset_basis = static_cast<std::uint32_t>(mhdr->payload);
      if (header.mcin)
      {
        if (static_cast<std::size_t>(header.mcin) + mhdr->payload + 8 == mcin->payload)
          model.mhdr_offset_basis = static_cast<std::uint32_t>(mhdr->payload);
        else if (static_cast<std::size_t>(header.mcin) + 0x14 + 8 == mcin->payload)
          model.mhdr_offset_basis = 0x14;
        else
        {
          warn(issues, "legacy ADT MHDR.mcin does not match the MCIN chunk position;"
                       " using the MHDR payload basis");
        }
      }
      auto mhdr_abs = [&](std::uint32_t stored) -> std::size_t
      {
        return stored ? static_cast<std::size_t>(stored) + model.mhdr_offset_basis : 0;
      };

      auto chunk_at = [&](std::uint32_t stored) -> RawChunk const*
      {
        std::size_t const abs = mhdr_abs(stored);
        if (!abs || abs + 8 > bytes.size())
          return nullptr;
        auto const it = std::find_if(top.begin(), top.end(),
                                     [abs](RawChunk const& c) { return c.start() == abs; });
        return it == top.end() ? nullptr : &*it;
      };

      if (RawChunk const* mtex = chunk_at(header.mtex))
      {
        if (mtex->magic == FC_MTEX)
        {
          model.textures = parseStringTable(bytes.data() + mtex->payload, mtex->size);
          std::size_t offset = 0;
          for (std::size_t i = 0; i < model.textures.size(); ++i)
          {
            model.texture_offsets[static_cast<std::uint32_t>(offset)] = i;
            offset += model.textures[i].size() + 1;
          }
        }
      }

      RawChunk const* mmdx = chunk_at(header.mmdx);
      RawChunk const* mmid = chunk_at(header.mmid);
      if (mmdx && mmid && mmdx->magic == FC_MMDX && mmid->magic == FC_MMID)
      {
        model.m2_names = resolveOffsetNames(bytes.data() + mmdx->payload, mmdx->size,
                                            bytes.data() + mmid->payload, mmid->size,
                                            issues, "legacy ADT MMDX/MMID");
      }
      RawChunk const* mwmo = chunk_at(header.mwmo);
      RawChunk const* mwid = chunk_at(header.mwid);
      if (mwmo && mwid && mwmo->magic == FC_MWMO && mwid->magic == FC_MWID)
      {
        model.wmo_names = resolveOffsetNames(bytes.data() + mwmo->payload, mwmo->size,
                                             bytes.data() + mwid->payload, mwid->size,
                                             issues, "legacy ADT MWMO/MWID");
      }

      if (RawChunk const* mddf = chunk_at(header.mddf))
      {
        if (mddf->magic == FC_MDDF && mddf->size % sizeof(ENTRY_MDDF) == 0)
        {
          model.mddf.resize(mddf->size / sizeof(ENTRY_MDDF));
          if (!model.mddf.empty())
            std::memcpy(model.mddf.data(), bytes.data() + mddf->payload, mddf->size);
        }
      }
      if (RawChunk const* modf = chunk_at(header.modf))
      {
        if (modf->magic == FC_MODF && modf->size % sizeof(ENTRY_MODF) == 0)
        {
          model.modf.resize(modf->size / sizeof(ENTRY_MODF));
          if (!model.modf.empty())
            std::memcpy(model.modf.data(), bytes.data() + modf->payload, modf->size);
        }
      }

      if ((header.flags & 0x1u) != 0)
      {
        if (RawChunk const* mfbo = chunk_at(header.mfbo))
        {
          if (mfbo->magic == FC_MFBO)
          {
            model.has_mfbo = true;
            model.mfbo = *mfbo;
          }
        }
      }
      if (RawChunk const* mh2o = chunk_at(header.mh2o))
      {
        if (mh2o->magic == FC_MH2O)
        {
          model.has_mh2o = true;
          model.mh2o = *mh2o;
        }
      }
      model.has_mtxf = header.mtxf != 0;

      std::size_t loaded_chunks = 0;
      for (std::size_t i = 0; i < 256; ++i)
      {
        ENTRY_MCIN entry{};
        std::memcpy(&entry, bytes.data() + mcin->payload + i * sizeof(ENTRY_MCIN), sizeof(entry));

        LegacyChunkModel& chunk = model.chunks[i];
        chunk.mcin_flags = entry.flags;
        chunk.mcin_async_id = entry.asyncID;

        if (entry.offset == 0 || entry.offset + 8 + sizeof(MapChunkHeader) > bytes.size())
          continue;
        if (readMagic(bytes.data() + entry.offset) != FC_MCNK)
          continue;

        std::size_t const header_pos = entry.offset + 8;
        std::memcpy(&chunk.header, bytes.data() + header_pos, sizeof(MapChunkHeader));
        chunk.present = true;
        ++loaded_chunks;

        std::size_t const chunk_end = std::min<std::size_t>(
          bytes.size(),
          entry.size ? static_cast<std::size_t>(entry.offset) + entry.size : bytes.size());
        std::vector<RawChunk> subs;
        walkChunks(bytes.data(), header_pos + sizeof(MapChunkHeader), chunk_end,
                   subs, issues, "legacy MCNK", true);

        for (RawChunk const& sub : subs)
        {
          switch (sub.magic)
          {
            case FC_MCVT: chunk.mcvt = sub; chunk.has_mcvt = true; break;
            case FC_MCNR: chunk.mcnr = sub; chunk.has_mcnr = true; break;
            case FC_MCSH: chunk.mcsh = sub; chunk.has_mcsh = true; break;
            case FC_MCSE: chunk.mcse = sub; chunk.has_mcse = true; break;
            case FC_MCCV: chunk.mccv = sub; chunk.has_mccv = true; break;
            case FC_MCAL: chunk.mcal = sub; chunk.has_mcal = true; break;
            case FC_MCLQ: chunk.mclq = sub; chunk.has_mclq = true; break;
            // Legacy MCNKs keep their object references in a single MCRF chunk:
            // nDoodadRefs indices into MDDF followed by nMapObjRefs indices into
            // MODF. MCRD/MCRW are the modern split names and never appear here,
            // so matching on them meant has_refs was never set for a real file.
            case FC_MCRF: chunk.mcrf = sub; chunk.has_mcrf = true; chunk.has_refs = true; break;
            case FC_MCRD: case FC_MCRW: chunk.has_refs = true; break;
            default: break;
          }
        }

        // Texture layers: MCLY entries at chunk base + ofsLayer (+8 skips
        // the sub-chunk header), exactly like TextureSet's legacy load.
        std::uint32_t const nLayers =
          std::min<std::uint32_t>(chunk.header.nLayers,
                                  static_cast<std::uint32_t>(MAX_CHUNK_LAYERS));
        if (chunk.header.nLayers > MAX_CHUNK_LAYERS)
        {
          std::ostringstream message;
          message << "legacy chunk " << i << " declares " << chunk.header.nLayers
                  << " texture layers; only the first " << MAX_CHUNK_LAYERS << " convert";
          warn(issues, message.str());
        }
        if (nLayers && chunk.header.ofsLayer
            && entry.offset + chunk.header.ofsLayer + 8
                 + nLayers * sizeof(ENTRY_MCLY) <= chunk_end)
        {
          std::size_t const mcly_pos = entry.offset + chunk.header.ofsLayer;
          if (readMagic(bytes.data() + mcly_pos) == FC_MCLY)
          {
            chunk.layers.resize(nLayers);
            std::memcpy(chunk.layers.data(), bytes.data() + mcly_pos + 8,
                        nLayers * sizeof(ENTRY_MCLY));
          }
          else
          {
            std::ostringstream message;
            message << "legacy chunk " << i
                    << ": ofsLayer does not point at MCLY; layers dropped";
            warn(issues, message.str());
          }
        }
      }

      if (loaded_chunks == 0)
      {
        fail(issues, "legacy ADT has no readable MCNK chunks");
        return false;
      }

      model.ok = true;
      return true;
    }

    // ------------------------------------------------------------------
    // MCLQ -> MH2O (only needed legacy -> modern: MH2O itself passes
    // through untouched in both directions because it is structurally
    // identical since WotLK)
    // ------------------------------------------------------------------

    int mclqTypeToLiquidId(std::uint8_t mclq_type, bool green_lava)
    {
      switch (mclq_type)
      {
        case mclq_liquid_ocean: return LIQUID_OCEAN;
        case mclq_liquid_slime: return LIQUID_SLIME;
        case mclq_liquid_magma: return green_lava ? LIQUID_Green_Lava : LIQUID_MAGMA;
        case mclq_liquid_river: return LIQUID_WATER;
        default: return LIQUID_WATER;
      }
    }

    //! Basic type of a LiquidType id for the well-known rows (the same
    //! fallback table liquid_layer::changeLiquidID uses when the DBC is
    //! unavailable).
    int liquidBasicType(int liquid_id)
    {
      switch (liquid_id)
      {
        case LIQUID_OCEAN: case LIQUID_WMO_Ocean: return liquid_basic_types_ocean;
        case LIQUID_MAGMA: case LIQUID_Green_Lava: case LIQUID_WMO_Magma:
          return liquid_basic_types_magma;
        case LIQUID_SLIME: case LIQUID_WMO_Slime: case LIQUID_NAXX_SLIME:
          return liquid_basic_types_slime;
        default: return liquid_basic_types_water;
      }
    }

    //! LiquidVertexFormats: 0 = HEIGHT_DEPTH, 1 = HEIGHT_UV, 2 = DEPTH.
    int vertexFormatForLiquid(int liquid_id, float min_height, float max_height)
    {
      switch (liquidBasicType(liquid_id))
      {
        case liquid_basic_types_magma:
        case liquid_basic_types_slime:
          return 1;
        case liquid_basic_types_ocean:
          return (min_height == 0.0f && max_height == 0.0f) ? 2 : 0;
        default:
          return 0;
      }
    }

    struct Mh2oLayerOut
    {
      MH2O_Information info{};
      std::uint64_t mask = 0;
      bool filled = true;
      bool fatigue = false;
      std::vector<float> heights;
      std::vector<std::uint16_t> uvs;   // x,y interleaved (HEIGHT_UV)
      std::vector<std::uint8_t> depths; // HEIGHT_DEPTH / DEPTH
    };

    //! Build a top-level MH2O chunk payload (256 headers + per-chunk
    //! records) from the tiles' MCLQ blocks. Mirrors ChunkWater::save /
    //! liquid_layer::save emission.
    std::vector<std::uint8_t> buildMh2oFromMclq(std::uint8_t const* bytes,
                                                std::size_t source_size,
                                                LegacyAdtModel const& model,
                                                ConversionOptions const& options,
                                                std::vector<ConversionIssue>& issues)
    {
      ByteSink sink;
      std::size_t const headers_pos = sink.position();
      sink.zeros(256 * sizeof(MH2O_Header));
      std::vector<MH2O_Header> headers(256);

      unsigned converted_blocks = 0;

      for (std::size_t chunk_index = 0; chunk_index < 256; ++chunk_index)
      {
        LegacyChunkModel const& chunk = model.chunks[chunk_index];
        if (!chunk.present || !chunk.has_mclq)
          continue;

        std::size_t block_bytes = chunk.mclq.size;
        if (block_bytes == 0 && chunk.header.sizeLiquid >= 8)
          block_bytes = chunk.header.sizeLiquid - 8; // MCLQ size-0 quirk
        std::size_t const block_count = block_bytes / sizeof(mclq);
        if (block_count == 0
            || chunk.mclq.payload + block_count * sizeof(mclq) > source_size)
          continue;

        std::vector<Mh2oLayerOut> layers;
        MH2O_Attributes attributes{};
        attributes.fishable = 0;
        bool any_attributes = false;

        for (std::size_t b = 0; b < block_count; ++b)
        {
          mclq liquid{};
          std::memcpy(&liquid, bytes + chunk.mclq.payload + b * sizeof(mclq), sizeof(mclq));

          std::uint8_t mclq_type = 0;
          bool has_render = false;
          for (int t = 0; t < 64; ++t)
          {
            mclq_tile const& tile = liquid.tiles[t];
            if (tile.fishable || tile.fatigue)
              any_attributes = true;
            if (tile.fishable)
              attributes.fishable |= (1ull << t);
            if (tile.fatigue)
              attributes.fatigue |= (1ull << t);
            if (!tile.dont_render)
            {
              mclq_type = tile.liquid_type;
              has_render = true;
            }
          }
          if (!has_render)
            continue;

          Mh2oLayerOut layer;
          int const liquid_id = mclqTypeToLiquidId(mclq_type, options.green_lava);
          int const vertex_format =
            vertexFormatForLiquid(liquid_id, liquid.min_height, liquid.max_height);
          layer.info.liquid_id = static_cast<std::uint16_t>(liquid_id);
          layer.info.liquid_vertex_format = static_cast<std::uint16_t>(vertex_format);
          layer.info.minHeight = liquid.min_height;
          layer.info.maxHeight = liquid.max_height;

          int min_x = 9, min_z = 9, max_x = 0, max_z = 0;
          for (int z = 0; z < 8; ++z)
          {
            for (int x = 0; x < 8; ++x)
            {
              mclq_tile const& tile = liquid.tiles[z * 8 + x];
              if (!tile.dont_render)
              {
                min_x = std::min(x, min_x);
                min_z = std::min(z, min_z);
                max_x = std::max(x + 1, max_x);
                max_z = std::max(z + 1, max_z);
              }
              else
              {
                layer.filled = false;
              }
              if (tile.fatigue)
                layer.fatigue = true;
            }
          }
          if (max_x <= min_x || max_z <= min_z)
            continue;

          layer.info.xOffset = static_cast<std::uint8_t>(min_x);
          layer.info.yOffset = static_cast<std::uint8_t>(min_z);
          layer.info.width = static_cast<std::uint8_t>(max_x - min_x);
          layer.info.height = static_cast<std::uint8_t>(max_z - min_z);

          if (!layer.filled)
          {
            std::uint64_t value = 1;
            for (int z = min_z; z < max_z; ++z)
              for (int x = min_x; x < max_x; ++x)
              {
                if (!liquid.tiles[z * 8 + x].dont_render)
                  layer.mask |= value;
                value <<= 1;
              }
          }

          if (vertex_format != 2)
          {
            layer.heights.reserve((layer.info.width + 1) * (layer.info.height + 1));
            for (int z = min_z; z <= max_z; ++z)
              for (int x = min_x; x <= max_x; ++x)
              {
                float h = liquid.vertices[z * 9 + x].height;
                h = std::max(liquid.min_height, std::min(h, liquid.max_height));
                layer.heights.push_back(h);
              }
          }
          if (vertex_format == 1)
          {
            for (int z = min_z; z <= max_z; ++z)
              for (int x = min_x; x <= max_x; ++x)
              {
                layer.uvs.push_back(liquid.vertices[z * 9 + x].magma.x);
                layer.uvs.push_back(liquid.vertices[z * 9 + x].magma.y);
              }
          }
          if (vertex_format == 0 || (vertex_format == 2 && !layer.fatigue))
          {
            for (int z = min_z; z <= max_z; ++z)
              for (int x = min_x; x <= max_x; ++x)
                layer.depths.push_back(liquid.vertices[z * 9 + x].water.depth);
          }

          layers.push_back(std::move(layer));
          ++converted_blocks;
        }

        if (layers.empty())
          continue;

        MH2O_Header& header = headers[chunk_index];
        header.nLayers = static_cast<std::uint32_t>(layers.size());

        if (any_attributes)
        {
          header.ofsAttributes = static_cast<std::uint32_t>(sink.position() - headers_pos);
          sink.raw(&attributes, sizeof(attributes));
        }

        header.ofsInformation = static_cast<std::uint32_t>(sink.position() - headers_pos);
        std::size_t const info_pos = sink.position();
        sink.zeros(sizeof(MH2O_Information) * layers.size());

        for (std::size_t li = 0; li < layers.size(); ++li)
        {
          Mh2oLayerOut const& layer = layers[li];
          MH2O_Information info = layer.info;

          if (!layer.filled && layer.mask != 0)
          {
            info.ofsInfoMask = static_cast<std::uint32_t>(sink.position() - headers_pos);
            sink.u32(static_cast<std::uint32_t>(layer.mask & 0xFFFFFFFFu));
            sink.u32(static_cast<std::uint32_t>(layer.mask >> 32));
          }

          if (!layer.heights.empty())
          {
            info.ofsHeightMap = static_cast<std::uint32_t>(sink.position() - headers_pos);
            for (float h : layer.heights)
              sink.f32(h);
          }
          if (!layer.uvs.empty())
          {
            for (std::uint16_t v : layer.uvs)
              sink.u16(v);
          }
          // Format 2 (DEPTH) with fatigue writes no plane at all
          // (liquid_layer::save skips it); depths are then already empty.
          if (!layer.depths.empty() && layer.info.liquid_vertex_format != 1)
          {
            if (layer.heights.empty())
              info.ofsHeightMap = static_cast<std::uint32_t>(sink.position() - headers_pos);
            for (std::uint8_t d : layer.depths)
              sink.u8(d);
          }

          std::memcpy(sink.bytes.data() + info_pos + li * sizeof(MH2O_Information),
                      &info, sizeof(info));
        }
      }

      for (std::size_t i = 0; i < 256; ++i)
      {
        std::memcpy(sink.bytes.data() + headers_pos + i * sizeof(MH2O_Header),
                    &headers[i], sizeof(MH2O_Header));
      }

      if (converted_blocks)
      {
        std::ostringstream message;
        message << "converted " << converted_blocks << " MCLQ liquid blocks to MH2O";
        warn(issues, message.str());
      }
      return sink.bytes;
    }

    // ------------------------------------------------------------------
    // Name / FileDataID helpers
    // ------------------------------------------------------------------

    std::string normalizeName(std::string const& name)
    {
      std::string result = name;
      for (char& c : result)
      {
        if (c == '/')
          c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return result;
    }

    //! Thin wrapper so ADT code can pass a role with each lookup; the
    //! caching and listfile preference live in FdidAllocator::resolve.
    class FdidResolver
    {
    public:
      FdidResolver(ConversionFileSource const& source, FdidAllocator& fdids)
        : _source(source), _fdids(fdids) {}

      std::uint32_t resolve(std::string const& raw_name, char const* role)
      {
        return _fdids.resolve(raw_name, role, _source);
      }

    private:
      ConversionFileSource const& _source;
      FdidAllocator& _fdids;
    };

    // ------------------------------------------------------------------
    // Legacy -> modern part assembly
    // ------------------------------------------------------------------

    std::vector<std::uint8_t> buildModernRoot(std::uint8_t const* source_bytes,
                                              LegacyAdtModel const& model,
                                              std::vector<std::uint8_t> const& mh2o_payload,
                                              std::vector<ConversionIssue>& issues)
    {
      ByteSink out;

      out.chunkHeader(FC_MVER, 4);
      out.u32(18);

      out.chunkHeader(FC_MHDR, 0x40);
      std::size_t const mhdr_payload_pos = out.position();
      out.zeros(0x40);
      MHDR mhdr{};
      mhdr.flags = model.mhdr_flags & 0x1u; // MFBO; legacy unknown bits dropped

      std::size_t const mcin_start = out.position();
      mhdr.mcin = static_cast<std::uint32_t>(mcin_start - mhdr_payload_pos);
      out.chunkHeader(FC_MCIN, 256 * sizeof(ENTRY_MCIN));
      std::size_t const mcin_payload = out.position();
      out.zeros(256 * sizeof(ENTRY_MCIN));

      if (model.has_mfbo)
      {
        mhdr.mfbo = static_cast<std::uint32_t>(out.position() - mhdr_payload_pos);
        out.chunkFrom(FC_MFBO, source_bytes + model.mfbo.payload, model.mfbo.size);
      }

      std::vector<ENTRY_MCIN> mcin_entries(256);

      for (std::size_t i = 0; i < 256; ++i)
      {
        LegacyChunkModel const& chunk = model.chunks[i];
        if (!chunk.present)
          continue;

        std::size_t const chunk_start = out.position();
        out.chunkHeader(FC_MCNK, 0); // size patched below

        MapChunkHeader header = chunk.header;
        header.nLayers = static_cast<std::uint32_t>(chunk.layers.size());
        header.nDoodadRefs = 0;
        header.nMapObjRefs = 0;
        header.ofsLayer = 0;
        header.ofsRefs = 0;
        header.ofsAlpha = 0;
        header.sizeAlpha = 0;
        header.ofsLiquid = 0;
        header.sizeLiquid = 0;
        header.ofsMCCV = 0;
        header.ofsShadow = 0;
        header.sizeShadow = 0;
        header.ofsSndEmitters = 0;
        header.nSndEmitters = 0;

        std::size_t const header_pos = out.position();
        out.zeros(sizeof(MapChunkHeader));

        auto emit_sub = [&](std::uint32_t magic, RawChunk const& sub, std::uint32_t& ofs_field)
        {
          ofs_field = static_cast<std::uint32_t>(out.position() - chunk_start);
          out.chunkFrom(magic, source_bytes + sub.payload, sub.size);
        };

        if (chunk.has_mcvt)
          emit_sub(FC_MCVT, chunk.mcvt, header.ofsHeight);
        if (chunk.has_mcnr)
          emit_sub(FC_MCNR, chunk.mcnr, header.ofsNormal);
        if (chunk.has_mccv)
          emit_sub(FC_MCCV, chunk.mccv, header.ofsMCCV);
        // The shadow map belongs to TEX0 in the split format: a retail 9.2.7
        // ROOT MCNK carries MCVT, MCNR and MCSE only, and MCSH sits in the
        // matching TEX0 chunk. Emitting it into ROOT while setting has_mcsh
        // sent the client looking for a shadow map in a TEX0 that never got
        // one, which is a read through a table that is not there. The bit
        // follows the legacy chunk's presence; TEX0 receives the chunk itself.
        header.flags.flags.has_mcsh =
          (chunk.has_mcsh && chunk.header.sizeShadow != 0) ? 1u : 0u;
        if (chunk.has_mcse && chunk.header.nSndEmitters)
        {
          emit_sub(FC_MCSE, chunk.mcse, header.ofsSndEmitters);
          header.nSndEmitters = chunk.header.nSndEmitters;
        }

        if (chunk.has_refs)
        {
          std::ostringstream message;
          message << "chunk " << i << ": per-chunk MCRD/MCRW reference lists are not"
                     " carried into the modern ROOT (placements attach per tile)";
          warn(issues, message.str());
        }

        // Legacy headers carry MCLQ-era liquid bits and a shadow bit, but this
        // tile emits MH2O only when the legacy actually had MCLQ to convert
        // (Noggit's exporter leaves it off by default) and MCSH only when the
        // legacy carried one. A bit set for a chunk that is not emitted sends the
        // client to a liquid or shadow table that does not exist, which it
        // dereferences: an access violation on entering any area whose chunks
        // claimed water. Clear the bits together with the chunk they describe.
        if (mh2o_payload.empty())
          header.flags.value &= ~legacy_liquid_flags;

        std::memcpy(out.bytes.data() + header_pos, &header, sizeof(header));

        std::size_t const chunk_end = out.position();
        out.patchU32(chunk_start + 4,
                     static_cast<std::uint32_t>(chunk_end - chunk_start - 8));
        mcin_entries[i].offset = static_cast<std::uint32_t>(chunk_start);
        mcin_entries[i].size = static_cast<std::uint32_t>(chunk_end - chunk_start);
        mcin_entries[i].flags = chunk.mcin_flags;
        mcin_entries[i].asyncID = chunk.mcin_async_id;
      }

      if (!mh2o_payload.empty())
      {
        mhdr.mh2o = static_cast<std::uint32_t>(out.position() - mhdr_payload_pos);
        out.chunkFrom(FC_MH2O, mh2o_payload.data(), mh2o_payload.size());
      }

      std::memcpy(out.bytes.data() + mhdr_payload_pos, &mhdr, sizeof(mhdr));
      for (std::size_t i = 0; i < 256; ++i)
      {
        std::memcpy(out.bytes.data() + mcin_payload + i * sizeof(ENTRY_MCIN),
                    &mcin_entries[i], sizeof(ENTRY_MCIN));
      }

      if (model.has_mtxf)
        warn(issues, "legacy MTXF (cube-map flags) has no modern ROOT equivalent; dropped");

      return out.bytes;
    }

    std::vector<std::uint8_t> buildModernTex0(std::uint8_t const* source_bytes,
                                              LegacyAdtModel const& model,
                                              std::vector<std::uint32_t> const& texture_fdids,
                                              bool big_alpha,
                                              std::vector<ConversionIssue>& issues)
    {
      ByteSink out;

      out.chunkHeader(FC_MVER, 4);
      out.u32(18);

      out.chunkHeader(FC_MDID, static_cast<std::uint32_t>(texture_fdids.size() * 4));
      for (std::uint32_t fdid : texture_fdids)
        out.u32(fdid);

      for (std::size_t i = 0; i < 256; ++i)
      {
        LegacyChunkModel const& chunk = model.chunks[i];

        std::size_t const chunk_start = out.position();
        out.chunkHeader(FC_MCNK, 0); // patched below

        // Build this chunk's MCAL first so the MCLY offsets are known.
        ByteSink mcal;
        std::vector<ENTRY_MCLY> entries = chunk.layers;

        for (std::size_t layer_index = 0; layer_index < entries.size(); ++layer_index)
        {
          ENTRY_MCLY& entry = entries[layer_index];

          // textureID: MTEX byte offset -> MDID index.
          auto const it = model.texture_offsets.find(entry.textureID);
          if (it == model.texture_offsets.end())
          {
            std::ostringstream message;
            message << "chunk " << i << " layer " << layer_index
                    << ": MCLY texture offset " << entry.textureID
                    << " is not in MTEX; remapped to texture 0";
            warn(issues, message.str());
            entry.textureID = 0;
          }
          else
          {
            entry.textureID = static_cast<std::uint32_t>(it->second);
          }

          if (layer_index == 0 || !(entry.flags & MCLY_FLAG_USE_ALPHA))
          {
            entry.flags &= ~MCLY_FLAG_ALPHA_COMPRESSED;
            entry.ofsAlpha = 0;
            continue;
          }

          // Legacy alpha span: from this layer's ofsAlpha to the next
          // alpha layer's (or the end of the chunk's MCAL).
          std::uint32_t alpha_end = chunk.has_mcal ? chunk.mcal.size : 0;
          for (std::size_t next = layer_index + 1; next < entries.size(); ++next)
          {
            if ((chunk.layers[next].flags & MCLY_FLAG_USE_ALPHA)
                && chunk.layers[next].ofsAlpha >= chunk.layers[layer_index].ofsAlpha)
            {
              alpha_end = chunk.layers[next].ofsAlpha;
              break;
            }
          }

          std::vector<std::uint8_t> plane;
          bool decoded = false;
          if (chunk.has_mcal
              && chunk.layers[layer_index].ofsAlpha < chunk.mcal.size
              && alpha_end <= chunk.mcal.size
              && alpha_end >= chunk.layers[layer_index].ofsAlpha)
          {
            std::ostringstream context;
            context << "chunk " << i << " layer " << layer_index;
            decoded = decodeAlphaPlane(source_bytes + chunk.mcal.payload
                                         + chunk.layers[layer_index].ofsAlpha,
                                       alpha_end - chunk.layers[layer_index].ofsAlpha,
                                       chunk.layers[layer_index].flags,
                                       big_alpha,
                                       plane,
                                       issues,
                                       context.str().c_str());
          }

          if (!decoded)
          {
            std::ostringstream message;
            message << "chunk " << i << " layer " << layer_index
                    << ": no readable legacy alpha; writing an opaque plane";
            warn(issues, message.str());
            plane.assign(4096, 255);
          }

          std::vector<std::uint8_t> const rle = encodeAlphaRle(plane);
          entry.ofsAlpha = static_cast<std::uint32_t>(mcal.position());
          if (!rle.empty())
          {
            entry.flags |= MCLY_FLAG_ALPHA_COMPRESSED;
            mcal.raw(rle.data(), rle.size());
          }
          else
          {
            entry.flags &= ~MCLY_FLAG_ALPHA_COMPRESSED;
            mcal.raw(plane.data(), plane.size());
          }
        }

        out.chunkHeader(FC_MCLY, static_cast<std::uint32_t>(entries.size() * sizeof(ENTRY_MCLY)));
        for (ENTRY_MCLY const& entry : entries)
          out.raw(&entry, sizeof(entry));

        out.chunkHeader(FC_MCAL, static_cast<std::uint32_t>(mcal.bytes.size()));
        out.raw(mcal.bytes.data(), mcal.bytes.size());

        // The shadow map of this chunk, where the split format and retail keep
        // it. The ROOT's has_mcsh bit is set exactly when this is emitted.
        if (chunk.has_mcsh && chunk.header.sizeShadow)
          out.chunkFrom(FC_MCSH, source_bytes + chunk.mcsh.payload, chunk.mcsh.size);

        out.patchU32(chunk_start + 4,
                     static_cast<std::uint32_t>(out.position() - chunk_start - 8));
      }

      return out.bytes;
    }

    std::vector<std::uint8_t> buildModernObj0(std::vector<std::uint8_t> const& legacy_adt,
                                              LegacyAdtModel const& model,
                                              FdidResolver& resolver,
                                              std::vector<ConversionIssue>& issues)
    {
      ByteSink out;

      out.chunkHeader(FC_MVER, 4);
      out.u32(18);

      // An unresolvable placement is dropped, which shifts the index of every
      // placement after it. The per-chunk references below are indices into
      // these lists, so each one has to be remapped through the tables built
      // here or it would point at the wrong object - or past the end.
      std::vector<ENTRY_MDDF> mddf_out;
      std::vector<std::int32_t> mddf_remap(model.mddf.size(), -1);
      mddf_out.reserve(model.mddf.size());
      for (std::size_t i = 0; i < model.mddf.size(); ++i)
      {
        ENTRY_MDDF entry = model.mddf[i];
        if (entry.nameID >= model.m2_names.size() || model.m2_names[entry.nameID].empty())
        {
          std::ostringstream message;
          message << "MDDF entry " << i << " references doodad id " << entry.nameID
                  << " without a resolvable MMDX name; entry dropped";
          warn(issues, message.str());
          continue;
        }
        entry.nameID = resolver.resolve(model.m2_names[entry.nameID], "m2");
        mddf_remap[i] = static_cast<std::int32_t>(mddf_out.size());
        mddf_out.push_back(entry);
      }

      std::vector<ENTRY_MODF> modf_out;
      std::vector<std::int32_t> modf_remap(model.modf.size(), -1);
      modf_out.reserve(model.modf.size());
      for (std::size_t i = 0; i < model.modf.size(); ++i)
      {
        ENTRY_MODF entry = model.modf[i];
        if (entry.nameID >= model.wmo_names.size() || model.wmo_names[entry.nameID].empty())
        {
          std::ostringstream message;
          message << "MODF entry " << i << " references WMO id " << entry.nameID
                  << " without a resolvable MWMO name; entry dropped";
          warn(issues, message.str());
          continue;
        }
        entry.nameID = resolver.resolve(model.wmo_names[entry.nameID], "wmo");
        entry.flags |= MODF_FLAG_NAME_IS_FDID;
        modf_remap[i] = static_cast<std::int32_t>(modf_out.size());
        modf_out.push_back(entry);
      }

      out.chunkHeader(FC_MDDF, static_cast<std::uint32_t>(mddf_out.size() * sizeof(ENTRY_MDDF)));
      for (ENTRY_MDDF const& entry : mddf_out)
        out.raw(&entry, sizeof(entry));

      out.chunkHeader(FC_MODF, static_cast<std::uint32_t>(modf_out.size() * sizeof(ENTRY_MODF)));
      for (ENTRY_MODF const& entry : modf_out)
        out.raw(&entry, sizeof(entry));

      // 256 MCNK chunks, each carrying that map chunk's object references as
      // MCRD (indices into MDDF) and MCRW (indices into MODF). A modern OBJ0
      // without them has placements that belong to no chunk, and the client
      // renders none of them - the objects silently vanish in game even though
      // the file parses fine.
      auto const remap_refs = [&issues](std::vector<std::uint32_t> const& source,
                                        std::vector<std::int32_t> const& remap,
                                        char const* what,
                                        std::size_t chunk_index)
      {
        std::vector<std::uint32_t> kept;
        kept.reserve(source.size());
        for (std::uint32_t index : source)
        {
          if (index >= remap.size())
          {
            std::ostringstream message;
            message << "chunk " << chunk_index << " has a " << what
                    << " reference to placement " << index
                    << ", which does not exist; reference dropped";
            warn(issues, message.str());
            continue;
          }
          if (remap[index] < 0)
            continue; // its placement was dropped above
          kept.push_back(static_cast<std::uint32_t>(remap[index]));
        }
        return kept;
      };

      for (std::size_t i = 0; i < 256; ++i)
      {
        LegacyChunkModel const& chunk = model.chunks[i];
        std::vector<std::uint32_t> doodad_refs;
        std::vector<std::uint32_t> wmo_refs;

        std::uint32_t const n_doodad = chunk.present ? chunk.header.nDoodadRefs : 0;
        std::uint32_t const n_wmo = chunk.present ? chunk.header.nMapObjRefs : 0;

        if (n_doodad || n_wmo)
        {
          std::size_t const needed = static_cast<std::size_t>(n_doodad + n_wmo) * 4;
          if (!chunk.has_mcrf)
          {
            std::ostringstream message;
            message << "chunk " << i << " declares " << n_doodad << " doodad and " << n_wmo
                    << " WMO reference(s) but has no MCRF chunk; references dropped";
            warn(issues, message.str());
          }
          else if (chunk.mcrf.size < needed
                   || chunk.mcrf.payload + needed > legacy_adt.size())
          {
            std::ostringstream message;
            message << "chunk " << i << " MCRF holds " << chunk.mcrf.size
                    << " byte(s), too few for its " << n_doodad << " doodad and " << n_wmo
                    << " WMO reference(s); references dropped";
            warn(issues, message.str());
          }
          else
          {
            auto read_ref = [&legacy_adt](std::size_t at)
            {
              std::uint32_t value;
              std::memcpy(&value, legacy_adt.data() + at, 4);
              return value;
            };
            for (std::uint32_t r = 0; r < n_doodad; ++r)
              doodad_refs.push_back(read_ref(chunk.mcrf.payload + r * 4));
            for (std::uint32_t r = 0; r < n_wmo; ++r)
              wmo_refs.push_back(read_ref(chunk.mcrf.payload
                                          + static_cast<std::size_t>(n_doodad + r) * 4));
          }
        }

        doodad_refs = remap_refs(doodad_refs, mddf_remap, "doodad", i);
        wmo_refs = remap_refs(wmo_refs, modf_remap, "WMO", i);

        ByteSink subs;
        if (!doodad_refs.empty())
        {
          subs.chunkHeader(FC_MCRD, static_cast<std::uint32_t>(doodad_refs.size() * 4));
          for (std::uint32_t value : doodad_refs)
            subs.u32(value);
        }
        if (!wmo_refs.empty())
        {
          subs.chunkHeader(FC_MCRW, static_cast<std::uint32_t>(wmo_refs.size() * 4));
          for (std::uint32_t value : wmo_refs)
            subs.u32(value);
        }

        out.chunkHeader(FC_MCNK, static_cast<std::uint32_t>(subs.position()));
        out.raw(subs.bytes.data(), subs.bytes.size());
      }

      return out.bytes;
    }

    // ------------------------------------------------------------------
    // Modern -> legacy assembly
    // ------------------------------------------------------------------

    std::string pathForFdid(std::uint32_t fdid,
                            char const* fallback_extension,
                            ConversionFileSource const& source,
                            std::vector<ConversionIssue>& issues,
                            bool& missing)
    {
      missing = false;
      if (source.fdid_to_path)
      {
        std::string const path = source.fdid_to_path(fdid);
        if (!path.empty())
          return normalizeName(path);
      }
      missing = true;
      std::ostringstream message;
      message << "FileDataID " << fdid << " is not in the source listfile; emitted as"
              << " a placeholder path (fix the asset or the listfile, then re-convert)";
      warn(issues, message.str());
      std::ostringstream placeholder;
      placeholder << "missing\\fdid_" << fdid << fallback_extension;
      return placeholder.str();
    }

    std::vector<std::uint8_t> buildLegacyAdt(std::vector<std::uint8_t> const& root,
                                             Noggit::Formats::ADT::ModernTex0Data const& tex0,
                                             Noggit::Formats::ADT::Obj0ParseResult const& obj0,
                                             ConversionOptions const& options,
                                             ConversionFileSource const& source,
                                             std::vector<ConversionIssue>& issues)
    {
      using Noggit::Formats::ADT::ModernTexChunk;
      using Noggit::Formats::ADT::ModernTextureLayer;

      std::vector<RawChunk> top;
      if (!walkChunks(root.data(), 0, root.size(), top, issues, "modern ROOT"))
        return {};

      RawChunk const* mhdr_chunk = findChunk(top, FC_MHDR);
      RawChunk const* mcin_chunk = findChunk(top, FC_MCIN);
      RawChunk const* mh2o_chunk = findChunk(top, FC_MH2O);
      RawChunk const* mfbo_chunk = findChunk(top, FC_MFBO);
      if (!mhdr_chunk || !mcin_chunk || mhdr_chunk->size < 0x40
          || mcin_chunk->size < 256 * sizeof(ENTRY_MCIN))
      {
        fail(issues, "modern ROOT has no usable MHDR/MCIN");
        return {};
      }
      MHDR source_mhdr{};
      std::memcpy(&source_mhdr, root.data() + mhdr_chunk->payload, sizeof(source_mhdr));

      std::vector<std::size_t> mcnk_offsets;
      std::vector<RawChunk> chunk_headers;
      for (RawChunk const& c : top)
      {
        if (c.magic == FC_MCNK)
        {
          mcnk_offsets.push_back(c.start());
          chunk_headers.push_back(c);
        }
      }
      if (mcnk_offsets.size() != 256)
      {
        std::ostringstream message;
        message << "modern ROOT has " << mcnk_offsets.size() << " MCNK chunks, expected 256";
        fail(issues, message.str());
        return {};
      }

      // ---- name tables from FileDataIDs ----
      ByteSink mtex;
      std::map<std::string, std::uint32_t> mtex_offsets;
      auto mtex_offset_for = [&](std::uint32_t fdid) -> std::uint32_t
      {
        bool missing = false;
        std::string path = pathForFdid(fdid, ".blp", source, issues, missing);
        auto const it = mtex_offsets.find(path);
        if (it != mtex_offsets.end())
          return it->second;
        std::uint32_t const offset = static_cast<std::uint32_t>(mtex.position());
        mtex.raw(path.c_str(), path.size() + 1);
        mtex_offsets.emplace(path, offset);
        return offset;
      };

      std::vector<std::uint32_t> texture_offsets_by_mdid;
      texture_offsets_by_mdid.reserve(tex0.diffuse_ids.size());
      for (std::uint32_t fdid : tex0.diffuse_ids)
        texture_offsets_by_mdid.push_back(mtex_offset_for(fdid));

      // MDDF -> MMDX/MMID (1:1 MMID entries, like legacy retail files).
      ByteSink mmdx;
      ByteSink mmid;
      std::vector<ENTRY_MDDF> mddf_out;
      mddf_out.reserve(obj0.m2_placements.size());
      for (std::size_t i = 0; i < obj0.m2_placements.size(); ++i)
      {
        ENTRY_MDDF entry = obj0.m2_placements[i];
        bool missing = false;
        std::string const path = pathForFdid(entry.nameID, ".m2", source, issues, missing);
        mmid.u32(static_cast<std::uint32_t>(mmdx.position()));
        mmdx.raw(path.c_str(), path.size() + 1);
        entry.nameID = static_cast<std::uint32_t>(i);
        mddf_out.push_back(entry);
      }

      ByteSink mwmo;
      ByteSink mwid;
      std::vector<ENTRY_MODF> modf_out;
      modf_out.reserve(obj0.wmo_placements.size());
      for (std::size_t i = 0; i < obj0.wmo_placements.size(); ++i)
      {
        ENTRY_MODF entry = obj0.wmo_placements[i];
        bool missing = false;
        std::string const path = pathForFdid(entry.nameID, ".wmo", source, issues, missing);
        mwid.u32(static_cast<std::uint32_t>(mwmo.position()));
        mwmo.raw(path.c_str(), path.size() + 1);
        entry.nameID = static_cast<std::uint32_t>(i);
        entry.flags &= ~MODF_FLAG_NAME_IS_FDID;
        modf_out.push_back(entry);
      }

      // ---- file skeleton ----
      ByteSink out;

      out.chunkHeader(FC_MVER, 4);
      out.u32(18);

      out.chunkHeader(FC_MHDR, 0x40);
      std::size_t const mhdr_payload_pos = out.position();
      out.zeros(0x40);
      MHDR mhdr{};
      mhdr.flags = source_mhdr.flags & 0x1u;
      auto mhdr_offset = [&](std::size_t chunk_start)
      {
        return static_cast<std::uint32_t>(chunk_start - mhdr_payload_pos);
      };

      mhdr.mcin = mhdr_offset(out.position());
      out.chunkHeader(FC_MCIN, 256 * sizeof(ENTRY_MCIN));
      std::size_t const mcin_payload = out.position();
      out.zeros(256 * sizeof(ENTRY_MCIN));

      mhdr.mtex = mhdr_offset(out.position());
      out.chunkHeader(FC_MTEX, static_cast<std::uint32_t>(mtex.bytes.size()));
      out.raw(mtex.bytes.data(), mtex.bytes.size());

      mhdr.mmdx = mhdr_offset(out.position());
      out.chunkHeader(FC_MMDX, static_cast<std::uint32_t>(mmdx.bytes.size()));
      out.raw(mmdx.bytes.data(), mmdx.bytes.size());

      mhdr.mmid = mhdr_offset(out.position());
      out.chunkHeader(FC_MMID, static_cast<std::uint32_t>(mmid.bytes.size()));
      out.raw(mmid.bytes.data(), mmid.bytes.size());

      mhdr.mwmo = mhdr_offset(out.position());
      out.chunkHeader(FC_MWMO, static_cast<std::uint32_t>(mwmo.bytes.size()));
      out.raw(mwmo.bytes.data(), mwmo.bytes.size());

      mhdr.mwid = mhdr_offset(out.position());
      out.chunkHeader(FC_MWID, static_cast<std::uint32_t>(mwid.bytes.size()));
      out.raw(mwid.bytes.data(), mwid.bytes.size());

      mhdr.mddf = mhdr_offset(out.position());
      out.chunkHeader(FC_MDDF,
                      static_cast<std::uint32_t>(mddf_out.size() * sizeof(ENTRY_MDDF)));
      for (ENTRY_MDDF const& entry : mddf_out)
        out.raw(&entry, sizeof(entry));

      mhdr.modf = mhdr_offset(out.position());
      out.chunkHeader(FC_MODF,
                      static_cast<std::uint32_t>(modf_out.size() * sizeof(ENTRY_MODF)));
      for (ENTRY_MODF const& entry : modf_out)
        out.raw(&entry, sizeof(entry));

      if (mfbo_chunk && (source_mhdr.flags & 0x1u))
      {
        mhdr.mfbo = mhdr_offset(out.position());
        out.chunkFrom(FC_MFBO, root.data() + mfbo_chunk->payload, mfbo_chunk->size);
      }

      std::vector<ENTRY_MCIN> mcin_entries(256);
      bool warned_mccv = false;
      bool warned_mtxp = false;
      bool warned_mcse = false;
      bool warned_small_alpha = false;

      for (std::size_t i = 0; i < 256; ++i)
      {
        std::size_t const source_chunk = mcnk_offsets[i];
        MapChunkHeader header{};
        std::memcpy(&header, root.data() + source_chunk + 8, sizeof(MapChunkHeader));

        ModernTexChunk const& tex_chunk =
          i < tex0.chunks.size() ? tex0.chunks[i] : ModernTexChunk{};

        std::uint32_t nLayers = static_cast<std::uint32_t>(tex_chunk.layers.size());
        if (nLayers > MAX_CHUNK_LAYERS)
        {
          std::ostringstream message;
          message << "chunk " << i << " has " << nLayers << " modern texture layers;"
                  << " clamped to " << MAX_CHUNK_LAYERS;
          warn(issues, message.str());
          nLayers = static_cast<std::uint32_t>(MAX_CHUNK_LAYERS);
        }

        std::size_t const chunk_start = out.position();
        out.chunkHeader(FC_MCNK, 0); // patched below
        std::size_t const header_pos = out.position();
        out.zeros(sizeof(MapChunkHeader));

        std::vector<RawChunk> subs;
        std::size_t const source_chunk_end =
          std::min(root.size(), source_chunk + 8 + chunk_headers[i].size);
        walkChunks(root.data(), source_chunk + 8 + sizeof(MapChunkHeader), source_chunk_end,
                   subs, issues, "modern ROOT MCNK");

        header.nLayers = nLayers;
        header.nDoodadRefs = 0;
        header.nMapObjRefs = 0;
        header.ofsRefs = 0;
        header.ofsLiquid = 0;
        header.sizeLiquid = 0;
        header.ofsMCCV = 0;
        header.ofsShadow = 0;
        header.sizeShadow = 0;
        header.ofsSndEmitters = 0;

        auto copy_sub = [&](std::uint32_t magic, RawChunk const& sub, std::uint32_t& ofs_field)
        {
          ofs_field = static_cast<std::uint32_t>(out.position() - chunk_start);
          out.chunkFrom(magic, root.data() + sub.payload, sub.size);
        };

        for (RawChunk const& sub : subs)
        {
          switch (sub.magic)
          {
            case FC_MCVT: copy_sub(FC_MCVT, sub, header.ofsHeight); break;
            case FC_MCNR: copy_sub(FC_MCNR, sub, header.ofsNormal); break;
            case FC_MCSH:
              copy_sub(FC_MCSH, sub, header.ofsShadow);
              header.sizeShadow = sub.size;
              break;
            case FC_MCSE:
              copy_sub(FC_MCSE, sub, header.ofsSndEmitters);
              if (!warned_mcse)
              {
                warned_mcse = true;
                warn(issues, "MCSE sound emitter ids reference the modern sound table;"
                             " 3.3.5 uses SoundEntries ids - verify emitters in game");
              }
              break;
            case FC_MCCV:
              if (!warned_mccv)
              {
                warned_mccv = true;
                warn(issues, "modern MCCV vertex shading has no WotLK equivalent; dropped");
              }
              break;
            default:
              break;
          }
        }

        // MCLY + MCAL from the TEX0 chunk.
        ByteSink mcal;
        std::vector<ENTRY_MCLY> entries;
        entries.reserve(nLayers);

        for (std::uint32_t layer_index = 0; layer_index < nLayers; ++layer_index)
        {
          ModernTextureLayer const& modern_layer = tex_chunk.layers[layer_index];
          ENTRY_MCLY entry{};

          if (modern_layer.texture_id < texture_offsets_by_mdid.size())
            entry.textureID = texture_offsets_by_mdid[modern_layer.texture_id];
          else
          {
            std::ostringstream message;
            message << "chunk " << i << " layer " << layer_index
                    << ": MCLY texture id " << modern_layer.texture_id
                    << " is outside MDID; remapped to texture 0";
            warn(issues, message.str());
            entry.textureID = texture_offsets_by_mdid.empty()
              ? 0 : texture_offsets_by_mdid[0];
          }

          entry.flags = modern_layer.flags & ~MCLY_FLAG_ALPHA_COMPRESSED;
          entry.effectID = 0xFFFFFFFFu;

          if (modern_layer.has_mtxp || modern_layer.height_file_data_id
              || modern_layer.has_height_id_table)
          {
            if (!warned_mtxp)
            {
              warned_mtxp = true;
              warn(issues, "modern height texturing (MHID/MTXP) has no WotLK equivalent;"
                           " dropped");
            }
          }

          if (layer_index > 0 && (entry.flags & MCLY_FLAG_USE_ALPHA))
          {
            std::vector<std::uint8_t> plane;
            if (!Noggit::Formats::ADT::decodeModernAlpha(tex_chunk, modern_layer,
                                                          layer_index, plane))
            {
              std::ostringstream message;
              message << "chunk " << i << " layer " << layer_index
                      << ": modern alpha failed to decode; writing an opaque plane";
              warn(issues, message.str());
              plane.assign(4096, 255);
            }
            entry.ofsAlpha = static_cast<std::uint32_t>(mcal.position());
            if (options.big_alpha)
              mcal.raw(plane.data(), plane.size());
            else
            {
              if (!warned_small_alpha)
              {
                warned_small_alpha = true;
                warn(issues, "emitting TBC-era 4-bit alpha maps (big_alpha off);"
                             " precision reduced");
              }
              std::vector<std::uint8_t> const packed = packSmallAlpha(plane);
              mcal.raw(packed.data(), packed.size());
            }
          }
          else
          {
            entry.ofsAlpha = 0;
          }

          entries.push_back(entry);
        }

        if (!entries.empty())
        {
          header.ofsLayer = static_cast<std::uint32_t>(out.position() - chunk_start);
          out.chunkHeader(FC_MCLY,
                          static_cast<std::uint32_t>(entries.size() * sizeof(ENTRY_MCLY)));
          for (ENTRY_MCLY const& entry : entries)
            out.raw(&entry, sizeof(entry));
        }
        else
        {
          header.ofsLayer = 0;
        }

        if (mcal.bytes.size())
        {
          header.ofsAlpha = static_cast<std::uint32_t>(out.position() - chunk_start);
          header.sizeAlpha = static_cast<std::uint32_t>(mcal.bytes.size());
          out.chunkHeader(FC_MCAL, static_cast<std::uint32_t>(mcal.bytes.size()));
          out.raw(mcal.bytes.data(), mcal.bytes.size());
        }
        else
        {
          header.ofsAlpha = 0;
          header.sizeAlpha = 0;
        }

        std::memcpy(out.bytes.data() + header_pos, &header, sizeof(header));

        std::size_t const chunk_end = out.position();
        out.patchU32(chunk_start + 4,
                     static_cast<std::uint32_t>(chunk_end - chunk_start - 8));

        ENTRY_MCIN mcin_entry{};
        std::memcpy(&mcin_entry,
                    root.data() + mcin_chunk->payload + i * sizeof(ENTRY_MCIN),
                    sizeof(mcin_entry));
        mcin_entries[i].offset = static_cast<std::uint32_t>(chunk_start);
        mcin_entries[i].size = static_cast<std::uint32_t>(chunk_end - chunk_start);
        mcin_entries[i].flags = mcin_entry.flags;
        mcin_entries[i].asyncID = mcin_entry.asyncID;
      }

      // MH2O last, like MapTile::save (3.3.5 reads the top-level MH2O).
      if (mh2o_chunk)
      {
        mhdr.mh2o = mhdr_offset(out.position());
        out.chunkFrom(FC_MH2O, root.data() + mh2o_chunk->payload, mh2o_chunk->size);
      }

      std::memcpy(out.bytes.data() + mhdr_payload_pos, &mhdr, sizeof(mhdr));
      for (std::size_t i = 0; i < 256; ++i)
      {
        std::memcpy(out.bytes.data() + mcin_payload + i * sizeof(ENTRY_MCIN),
                    &mcin_entries[i], sizeof(ENTRY_MCIN));
      }

      return out.bytes;
    }
  }

  using namespace adt_detail; // helpers shared with the public entry points below

  // --------------------------------------------------------------------
  // Public entry points
  // --------------------------------------------------------------------

  AdtConversionResult convertAdtLegacyToModern(std::vector<std::uint8_t> const& legacy_adt,
                                               ConversionOptions const& options,
                                               ConversionFileSource const& source,
                                               FdidAllocator& fdids)
  {
    AdtConversionResult result;
    LegacyAdtModel model;
    if (!parseLegacyAdt(legacy_adt, model, result.issues))
      return result;

    FdidResolver resolver(source, fdids);

    // MDID: one fdid per MTEX entry (index-preserving, so MCLY.textureID
    // becomes the same number the legacy offset mapped to).
    std::vector<std::uint32_t> texture_fdids;
    texture_fdids.reserve(model.textures.size());
    for (std::string const& name : model.textures)
      texture_fdids.push_back(resolver.resolve(name, "texture"));

    // Liquids: top-level MH2O passes through; MCLQ-only files get a
    // converted MH2O when requested.
    std::vector<std::uint8_t> mh2o_payload;
    bool any_mclq = false;
    for (LegacyChunkModel const& chunk : model.chunks)
      any_mclq = any_mclq || chunk.has_mclq;

    if (model.has_mh2o)
    {
      mh2o_payload.assign(legacy_adt.begin() + model.mh2o.payload,
                          legacy_adt.begin() + model.mh2o.payload + model.mh2o.size);
    }
    else if (any_mclq)
    {
      if (options.convert_liquids)
        mh2o_payload = buildMh2oFromMclq(legacy_adt.data(), legacy_adt.size(),
                                         model, options, result.issues);
      else
        warn(result.issues, "tile has MCLQ liquids and liquid conversion is off; liquids dropped");
    }

    result.root = buildModernRoot(legacy_adt.data(), model, mh2o_payload, result.issues);
    result.tex0 = buildModernTex0(legacy_adt.data(), model, texture_fdids,
                                  options.big_alpha, result.issues);
    result.obj0 = buildModernObj0(legacy_adt, model, resolver, result.issues);

    // Self-check with the verified modern readers.
    Noggit::Formats::ADT::AdtScanOptions scan_options;
    scan_options.mcnk_has_header = true;
    scan_options.validate_mcnk_subchunks = false;
    auto const scan = Noggit::Formats::ADT::scanAdt(result.root, scan_options);
    if (Noggit::Formats::ADT::classifyAdtLayout(scan)
        != Noggit::Formats::ADT::AdtLayout::modern_split_root)
      fail(result.issues, "converted ROOT does not classify as a modern split root");

    auto const tex0_check = Noggit::Formats::ADT::parseTex0(result.tex0);
    if (!tex0_check.ok)
      fail(result.issues, "converted TEX0 failed re-parse");

    auto const obj0_check = Noggit::Formats::ADT::parseObj0(result.obj0);
    if (!obj0_check.ok)
      fail(result.issues, "converted OBJ0 failed re-parse");

    for (auto const& issue : result.issues)
    {
      if (issue.severity == ConversionIssue::Severity::error)
        return result;
    }
    result.ok = true;
    return result;
  }

  AdtConversionResult convertAdtModernToLegacy(std::vector<std::uint8_t> const& root,
                                               std::vector<std::uint8_t> const& tex0,
                                               std::vector<std::uint8_t> const& obj0,
                                               std::vector<std::uint8_t> const& obj1,
                                               ConversionOptions const& options,
                                               ConversionFileSource const& source,
                                               FdidAllocator& fdids)
  {
    (void)fdids;
    AdtConversionResult result;

    if (root.empty() || tex0.empty())
    {
      fail(result.issues, "modern tile is missing its ROOT or TEX0 part");
      return result;
    }
    if (!obj1.empty())
      warn(result.issues, "modern _obj1 part (LOD placements) has no WotLK equivalent; dropped");

    auto const tex0_parsed = Noggit::Formats::ADT::parseTex0(tex0);
    if (!tex0_parsed.ok)
    {
      fail(result.issues, "modern TEX0 failed to parse; tile not convertible");
      for (auto const& issue : tex0_parsed.issues)
      {
        if (issue.severity == Noggit::Formats::ADT::AdtIssueSeverity::error)
          fail(result.issues, "TEX0: " + issue.message);
      }
      return result;
    }

    Noggit::Formats::ADT::Obj0ParseResult obj0_parsed;
    if (!obj0.empty())
    {
      obj0_parsed = Noggit::Formats::ADT::parseObj0(obj0);
      if (!obj0_parsed.ok)
      {
        fail(result.issues, "modern OBJ0 failed to parse; placements will be missing");
        obj0_parsed.m2_placements.clear();
        obj0_parsed.wmo_placements.clear();
      }
    }

    result.root = buildLegacyAdt(root, tex0_parsed.data, obj0_parsed,
                                 options, source, result.issues);
    if (result.root.empty())
      return result;

    // Self-check: re-parse the emitted monolith with the legacy walker.
    LegacyAdtModel check_model;
    std::vector<ConversionIssue> check_issues;
    if (!parseLegacyAdt(result.root, check_model, check_issues))
    {
      for (ConversionIssue const& issue : check_issues)
        fail(result.issues, "legacy re-parse: " + issue.message);
      return result;
    }

    for (auto const& issue : result.issues)
    {
      if (issue.severity == ConversionIssue::Severity::error)
        return result;
    }
    result.ok = true;
    return result;
  }
}
