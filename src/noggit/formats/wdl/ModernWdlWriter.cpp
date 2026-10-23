// Native modern (Legion+) WDL writer. See the header for the format facts
// and the reason this module exists. Qt-free, dependency-free.

#include <noggit/formats/wdl/ModernWdlWriter.hpp>

#include <cmath>
#include <cstring>
#include <sstream>

namespace Noggit::Formats::WDL
{
  namespace
  {
    constexpr std::uint32_t wdlw_fc(char a, char b, char c, char d) noexcept
    {
      return (std::uint32_t(static_cast<unsigned char>(a)) << 24)
           | (std::uint32_t(static_cast<unsigned char>(b)) << 16)
           | (std::uint32_t(static_cast<unsigned char>(c)) << 8)
           | std::uint32_t(static_cast<unsigned char>(d));
    }

    // Tags compare as the LE u32 of the MSB-first fourcc, the convention
    // every reader and writer in this tree shares.
    constexpr std::uint32_t WDLW_MVER = wdlw_fc('M', 'V', 'E', 'R');
    constexpr std::uint32_t WDLW_MAOF = wdlw_fc('M', 'A', 'O', 'F');
    constexpr std::uint32_t WDLW_MARE = wdlw_fc('M', 'A', 'R', 'E');
    constexpr std::uint32_t WDLW_MAHO = wdlw_fc('M', 'A', 'H', 'O');
    constexpr std::uint32_t WDLW_MLMD = wdlw_fc('M', 'L', 'M', 'D');
    constexpr std::uint32_t WDLW_MLMX = wdlw_fc('M', 'L', 'M', 'X');

    constexpr std::size_t WDLW_MARE_PAYLOAD = 17 * 17 * 2 + 16 * 16 * 2; // 1090
    constexpr std::size_t WDLW_MAHO_PAYLOAD = 16 * 2;                    // 32
    constexpr std::size_t WDLW_MAOF_PAYLOAD = 4096 * 4;

    // ENTRY_MODF without the 24 byte extents block, nameID = FileDataID.
    struct wdlw_mlmd_record
    {
      std::uint32_t nameID;
      std::uint32_t uniqueID;
      float pos[3];
      float rot[3];
      std::uint16_t flags;
      std::uint16_t doodadSet;
      std::uint16_t nameSet;
      std::uint16_t scale;
    };
    static_assert(sizeof(wdlw_mlmd_record) == 40, "MLMD records are 40 bytes");

    struct wdlw_mlmx_record
    {
      float min[3];
      float max[3];
      float radius;
    };
    static_assert(sizeof(wdlw_mlmx_record) == 28, "MLMX records are 28 bytes");

    // "modf entry is file data id" (Legion+). MLMD nameIDs always are
    // FileDataIDs, so the flag is cleared in the emitted record, matching
    // Conversion::buildModernWdl.
    constexpr std::uint16_t WDLW_MODF_FLAG_NAME_IS_FDID = 0x8;

    struct wdlw_sink
    {
      std::vector<std::uint8_t> bytes;

      std::size_t position() const { return bytes.size(); }

      void u32(std::uint32_t v)
      {
        for (int i = 0; i < 4; ++i)
          bytes.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
      }

      void raw(void const* p, std::size_t n)
      {
        auto const* s = static_cast<std::uint8_t const*>(p);
        bytes.insert(bytes.end(), s, s + n);
      }

      void zeros(std::size_t n) { bytes.resize(bytes.size() + n, 0); }

      void patch_u32(std::size_t at, std::uint32_t v)
      {
        for (int i = 0; i < 4; ++i)
          bytes[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
      }

      void chunk_header(std::uint32_t magic, std::uint32_t size)
      {
        u32(magic);
        u32(size);
      }
    };

    struct wdlw_chunk
    {
      std::uint32_t magic;
      std::size_t offset;
      std::size_t size;
    };

    //! Strict top-level walk: every byte must belong to a chunk.
    bool wdlw_walk(std::vector<std::uint8_t> const& bytes,
                   std::vector<wdlw_chunk>& out, std::string* error)
    {
      std::size_t pos = 0;
      while (pos + 8 <= bytes.size())
      {
        wdlw_chunk chunk{};
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

    std::string wdlw_chunk_text(std::uint32_t magic)
    {
      std::string text;
      for (int shift = 24; shift >= 0; shift -= 8)
      {
        auto const character = static_cast<char>((magic >> shift) & 0xFF);
        text += (character >= 32 && character < 127) ? character : '?';
      }
      return text;
    }

    std::uint32_t wdlw_read_u32(std::vector<std::uint8_t> const& bytes, std::size_t at)
    {
      std::uint32_t v = 0;
      std::memcpy(&v, bytes.data() + at, 4);
      return v;
    }

    void wdlw_emit_tile(wdlw_sink& out, ModernWdlTile const& tile,
                        std::vector<std::uint32_t>& maof, std::size_t index)
    {
      maof[index] = static_cast<std::uint32_t>(out.position());
      out.chunk_header(WDLW_MARE, static_cast<std::uint32_t>(WDLW_MARE_PAYLOAD));
      out.raw(tile.height_17, sizeof(tile.height_17));
      out.raw(tile.height_16, sizeof(tile.height_16));
      out.chunk_header(WDLW_MAHO, static_cast<std::uint32_t>(WDLW_MAHO_PAYLOAD));
      out.raw(tile.holes, sizeof(tile.holes));
    }

    void wdlw_emit_objects(wdlw_sink& out,
                           std::vector<ModernWdlObject> const& objects,
                           std::size_t& emitted, std::size_t& skipped)
    {
      wdlw_sink mlmd;
      wdlw_sink mlmx;
      for (auto const& object : objects)
      {
        if (!object.fdid)
        {
          ++skipped;
          continue;
        }
        ENTRY_MODF const& modf = object.placement;

        wdlw_mlmd_record record{};
        record.nameID = object.fdid;
        record.uniqueID = modf.uniqueID;
        std::memcpy(record.pos, modf.pos, sizeof(record.pos));
        std::memcpy(record.rot, modf.rot, sizeof(record.rot));
        record.flags = static_cast<std::uint16_t>(modf.flags & ~WDLW_MODF_FLAG_NAME_IS_FDID);
        record.doodadSet = modf.doodadSet;
        record.nameSet = modf.nameSet;
        record.scale = modf.scale;
        mlmd.raw(&record, sizeof(record));

        wdlw_mlmx_record bounds{};
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

        ++emitted;
      }

      if (mlmd.bytes.size())
      {
        out.chunk_header(WDLW_MLMD, static_cast<std::uint32_t>(mlmd.bytes.size()));
        out.raw(mlmd.bytes.data(), mlmd.bytes.size());
        out.chunk_header(WDLW_MLMX, static_cast<std::uint32_t>(mlmx.bytes.size()));
        out.raw(mlmx.bytes.data(), mlmx.bytes.size());
      }
    }
  }

  ModernWdlResult writeModernWdl(ModernWdlInput const& input)
  {
    ModernWdlResult result;
    wdlw_sink out;

    out.chunk_header(WDLW_MVER, 4);
    out.u32(18);

    out.chunk_header(WDLW_MAOF, static_cast<std::uint32_t>(WDLW_MAOF_PAYLOAD));
    std::size_t const maof_pos = out.position();
    std::vector<std::uint32_t> maof(4096, 0);
    out.zeros(WDLW_MAOF_PAYLOAD);

    std::size_t tiles_written = 0;
    for (auto const& entry : input.tiles)
    {
      if (entry.first >= 4096)
      {
        result.notes.push_back("tile index " + std::to_string(entry.first)
                               + " lies outside the 64x64 MAOF table; skipped");
        continue;
      }
      wdlw_emit_tile(out, entry.second, maof, entry.first);
      ++tiles_written;
    }

    for (std::size_t i = 0; i < 4096; ++i)
      out.patch_u32(maof_pos + i * 4, maof[i]);

    std::size_t objects_written = 0;
    std::size_t objects_skipped = 0;
    wdlw_emit_objects(out, input.objects, objects_written, objects_skipped);

    {
      std::ostringstream note;
      note << "wrote " << tiles_written << " MARE/MAHO tile(s) and "
           << objects_written << " horizon WMO placement(s) (MLMD/MLMX)";
      result.notes.push_back(note.str());
    }
    if (objects_skipped)
    {
      std::ostringstream note;
      note << objects_skipped
           << " horizon WMO placement(s) skipped: no resolved FileDataID"
              " (an id invented offline would dangle in the target client)";
      result.notes.push_back(note.str());
    }

    result.bytes = std::move(out.bytes);
    result.ok = true;
    return result;
  }

  ModernWdlResult rewriteModernWdl(WdlRewriteInput const& input)
  {
    ModernWdlResult result;

    std::vector<wdlw_chunk> chunks;
    std::string error;
    if (!wdlw_walk(input.original, chunks, &error))
    {
      result.notes.push_back("the original WDL does not walk, " + error
                             + "; refusing to rebuild it");
      return result;
    }

    std::uint32_t version = 18;
    bool have_mver = false;
    const wdlw_chunk* maof_chunk = nullptr;
    for (auto const& chunk : chunks)
    {
      if (chunk.magic == WDLW_MVER && chunk.size >= 4 && !have_mver)
      {
        version = wdlw_read_u32(input.original, chunk.offset + 8);
        have_mver = true;
      }
      else if (chunk.magic == WDLW_MAOF && !maof_chunk)
      {
        maof_chunk = &chunk;
      }
    }

    if (maof_chunk && maof_chunk->size < WDLW_MAOF_PAYLOAD)
    {
      result.notes.push_back("the original MAOF chunk is too small ("
                             + std::to_string(maof_chunk->size)
                             + " bytes); refusing to rebuild rather than lose tiles");
      return result;
    }

    // Original tile records: MAOF entry -> [MARE tag .. end of MARE], plus
    // the MAHO chunk when one directly follows. Offsets inside those ranges
    // are owned by tiles and re-emitted (or copied) through the tile loop;
    // everything else is preserved verbatim afterwards.
    struct original_tile
    {
      bool valid = false;
      std::size_t begin = 0;
      std::size_t end = 0;
    };
    std::vector<original_tile> originals(4096);
    std::vector<bool> owned(input.original.size() + 1, false);

    if (maof_chunk)
    {
      for (std::size_t i = 0; i < 4096; ++i)
      {
        std::uint32_t const offset =
          wdlw_read_u32(input.original, maof_chunk->offset + 8 + i * 4);
        if (!offset)
          continue;
        if (offset + 8 + WDLW_MARE_PAYLOAD > input.original.size()
            || wdlw_read_u32(input.original, offset) != WDLW_MARE)
        {
          result.notes.push_back("MAOF entry " + std::to_string(i)
                                 + " does not point at a readable MARE chunk;"
                                   " the tile is treated as absent");
          continue;
        }

        original_tile tile;
        tile.valid = true;
        tile.begin = offset;
        tile.end = offset + 8 + WDLW_MARE_PAYLOAD;
        // A MAHO chunk directly after the MARE belongs to the tile.
        if (tile.end + 8 + WDLW_MAHO_PAYLOAD <= input.original.size()
            && wdlw_read_u32(input.original, tile.end) == WDLW_MAHO
            && wdlw_read_u32(input.original, tile.end + 4) == WDLW_MAHO_PAYLOAD)
        {
          tile.end += 8 + WDLW_MAHO_PAYLOAD;
        }
        originals[i] = tile;
        for (std::size_t b = tile.begin; b < tile.end; ++b)
          owned[b] = true;
      }
    }

    wdlw_sink out;
    out.chunk_header(WDLW_MVER, 4);
    out.u32(version);
    out.chunk_header(WDLW_MAOF, static_cast<std::uint32_t>(WDLW_MAOF_PAYLOAD));
    std::size_t const maof_pos = out.position();
    std::vector<std::uint32_t> maof(4096, 0);
    out.zeros(WDLW_MAOF_PAYLOAD);

    std::size_t replaced = 0;
    std::size_t kept = 0;
    std::size_t added = 0;
    for (std::size_t i = 0; i < 4096; ++i)
    {
      auto const replacement = input.tiles.find(i);
      if (replacement != input.tiles.end())
      {
        wdlw_emit_tile(out, replacement->second, maof, i);
        originals[i].valid ? ++replaced : ++added;
        continue;
      }
      if (originals[i].valid)
      {
        maof[i] = static_cast<std::uint32_t>(out.position());
        out.raw(input.original.data() + originals[i].begin,
                originals[i].end - originals[i].begin);
        ++kept;
      }
    }

    for (std::size_t i = 0; i < 4096; ++i)
      out.patch_u32(maof_pos + i * 4, maof[i]);

    // Every chunk that is not MVER, MAOF or a tile record: copied through
    // byte for byte, original order. This is what keeps MLMD/MLMX, sky
    // scenes, ocean data and unknown future chunks alive across the rewrite.
    std::vector<std::string> preserved;
    for (auto const& chunk : chunks)
    {
      if (chunk.magic == WDLW_MVER || chunk.magic == WDLW_MAOF)
        continue;
      if (owned[chunk.offset])
        continue; // tile record, handled above
      out.raw(input.original.data() + chunk.offset, 8 + chunk.size);
      preserved.push_back(wdlw_chunk_text(chunk.magic));
    }

    {
      std::ostringstream note;
      note << "rebuilt the WDL: " << replaced << " tile(s) updated, " << added
           << " tile(s) added, " << kept << " tile(s) kept from the original";
      result.notes.push_back(note.str());
    }
    if (!preserved.empty())
    {
      std::ostringstream note;
      note << "preserved byte for byte: ";
      for (std::size_t i = 0; i < preserved.size(); ++i)
        note << preserved[i] << (i + 1 < preserved.size() ? ", " : "");
      result.notes.push_back(note.str());
    }

    result.bytes = std::move(out.bytes);
    result.ok = true;
    return result;
  }
}
