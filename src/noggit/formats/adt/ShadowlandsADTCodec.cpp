// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/formats/adt/ShadowlandsADTCodec.hpp>

#include <noggit/formats/adt/ModernAdtWriter.hpp>
#include <noggit/formats/conversion/MapConversion.hpp>

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/Log.h>

#include <ClientFile.hpp>
#include <ClientData.hpp>
#include <Listfile.hpp>

#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <optional>
#include <unordered_map>
#include <utility>

namespace Noggit::Formats::ADT
{
  namespace
  {
    struct ChunkView
    {
      std::uint32_t magic = 0;
      std::size_t offset = 0;
      std::size_t payload = 0;
      std::uint32_t size = 0;
    };

    constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept
    {
      return (static_cast<std::uint32_t>(a) << 24)
           | (static_cast<std::uint32_t>(b) << 16)
           | (static_cast<std::uint32_t>(c) << 8)
           | static_cast<std::uint32_t>(d);
    }

    constexpr std::size_t part_index(ShadowlandsADTPart part) noexcept
    {
      return static_cast<std::size_t>(part);
    }

    std::string split_path(std::string const& root_path, char const* suffix)
    {
      auto const extension = root_path.rfind(".adt");
      if (extension == std::string::npos)
        return root_path + suffix;

      return root_path.substr(0, extension) + suffix + ".adt";
    }

    //! Smallest byte count that can hold the MVER chunk every ADT part starts
    //! with: an 8 byte header plus its 4 byte payload.
    constexpr std::size_t minimum_adt_part_size = 12;

    //! Whether bytes can serve as the part they claim to be.
    //!
    //! A project override left over from another client build, or from an older
    //! Noggit that wrote a different shape, still "reads" perfectly well and then
    //! either fails every save of the tile or, worse, is carried into the output
    //! verbatim. The checks are structural and cheap: the part has to walk as
    //! chunks at all, carry MVER, and look like its split-format role. A legacy
    //! monolithic ADT parked at the ROOT path is told apart by the chunks that
    //! only exist before the split format - MTEX and MMDX moved into TEX0 and
    //! OBJ0, so a modern ROOT never has them.
    char const* part_name(ShadowlandsADTPart part)
    {
      switch (part)
      {
      case ShadowlandsADTPart::Root: return "ROOT";
      case ShadowlandsADTPart::Tex0: return "TEX0";
      case ShadowlandsADTPart::Obj0: return "OBJ0";
      case ShadowlandsADTPart::Obj1: return "OBJ1";
      case ShadowlandsADTPart::Lod: return "LOD";
      }
      return "ADT";
    }

    bool part_is_usable(std::vector<std::uint8_t> const& bytes, ShadowlandsADTPart part,
                        std::string* reason = nullptr)
    {
      auto const no = [reason](std::string text)
      {
        if (reason)
          *reason = std::move(text);
        return false;
      };

      try
      {
        auto const chunks = parse_chunks(bytes, "backing part");
        auto const has = [&chunks](std::uint32_t magic)
        {
          return find_chunk(chunks, magic) != nullptr;
        };

        if (!has(fourcc('M', 'V', 'E', 'R')))
          return no("it has no MVER chunk");

        switch (part)
        {
        case ShadowlandsADTPart::Root:
          if (!has(fourcc('M', 'H', 'D', 'R')))
            return no("it has no MHDR chunk");
          if (!has(fourcc('M', 'C', 'N', 'K')))
            return no("it has no MCNK chunk");
          // MCIN is deliberately NOT required: retail ROOT files may omit it
          // (the client walks MCNK sequentially), and the native writer now
          // synthesizes one. Requiring it rejected the CASC originals of 62
          // azeroth tiles and forced them down the lossy synthesized path.
          if (has(fourcc('M', 'T', 'E', 'X')) || has(fourcc('M', 'M', 'D', 'X')))
            return no("it carries MTEX/MMDX, i.e. a legacy monolithic ADT");
          return true;
        case ShadowlandsADTPart::Tex0:
        case ShadowlandsADTPart::Obj0:
        case ShadowlandsADTPart::Obj1:
          // MCNK is optional in the satellite parts: a retail OBJ1 (or OBJ0)
          // on a tile with no matching references carries no per-chunk
          // sections at all. Requiring MCNK rejected 89 genuine retail OBJ1
          // parts and their CASC originals alike.
          return true;
        case ShadowlandsADTPart::Lod:
          return true;
        }
        return no("the part role is unknown");
      }
      catch (std::exception const& error)
      {
        return no(std::string("it does not parse: ") + error.what());
      }
    }

    //! The CASC original of a part, by whichever key resolves.
    //!
    //! A FileKey built from a path alone carries no FileDataID unless the
    //! listfile resolved one at construction, and readFile on an unresolved key
    //! fails without a word - which is how the first version of this fallback
    //! silently did nothing at all. Try the deduced component key, the
    //! listfile-resolved path key and the plain FileDataID, and report which one
    //! worked so a failure can never be silent again.
    std::vector<std::uint8_t> read_casc_original(std::string const& path,
                                                 BlizzardArchive::ClientData* client_data,
                                                 std::string* how)
    {
      auto attempt = [client_data, how](BlizzardArchive::Listfile::FileKey key,
                                        char const* name)
      {
        std::vector<char> bytes;
        if (client_data->readFile(key, bytes) && !bytes.empty())
        {
          if (how)
            *how = name;
          return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
        }
        return std::vector<std::uint8_t>{};
      };

      BlizzardArchive::Listfile::FileKey deduced(path);
      deduced.deduceOtherComponent(client_data->listfile());
      if (auto bytes = attempt(deduced, "via the deduced component key"); !bytes.empty())
        return bytes;

      BlizzardArchive::Listfile::FileKey listed(path, client_data->listfile());
      if (auto bytes = attempt(listed, "via the listfile path key"); !bytes.empty())
        return bytes;

      if (auto const fdid = client_data->listfile()->getFileDataID(path))
      {
        if (auto bytes = attempt(BlizzardArchive::Listfile::FileKey(fdid),
                                 "via the listfile FileDataID"); !bytes.empty())
          return bytes;
      }

      return {};
    }

    ShadowlandsADTRawPart read_part(std::string const& path, bool required,
                                    ShadowlandsADTPart part)
    {
      auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();

      try
      {
        BlizzardArchive::Listfile::FileKey key(path);
        BlizzardArchive::ClientFile file(key, client_data);

        auto const size = file.getSize();

        // ClientFile prefers the loose project override, and an override that is
        // present but too small to hold a chunk header still "reads" fine. An
        // empty buffer walks as zero chunks, which surfaced much further down as
        // "Original ROOT lacks MVER/MHDR/MCIN" and then failed every save of that
        // tile, permanently: the stale override kept masking the real file.
        // Read the original back from CASC instead of trusting it.
        if (size < minimum_adt_part_size)
        {
          BlizzardArchive::Listfile::FileKey casc_key(path);
          casc_key.deduceOtherComponent(client_data->listfile());

          std::vector<char> casc_bytes;
          if (client_data->readFile(casc_key, casc_bytes)
              && casc_bytes.size() >= minimum_adt_part_size)
          {
            LogError << "[ModernADT][Save] The project override for '" << path << "' is "
                     << size << " byte(s), too small to be an ADT part, so the original "
                     << casc_bytes.size() << " byte(s) were read from CASC instead. Delete"
                        " that stale file if the override was not intentional." << std::endl;

            ShadowlandsADTRawPart result;
            result.present = true;
            result.bytes.assign(casc_bytes.begin(), casc_bytes.end());
            return result;
          }

          throw std::runtime_error("'" + path + "' is " + std::to_string(size)
                                   + " byte(s), too small to be an ADT part, and CASC has no"
                                     " usable copy of it either");
        }

        ShadowlandsADTRawPart result;
        result.present = true;
        auto const* buffer = reinterpret_cast<std::uint8_t const*>(file.getBuffer());
        result.bytes.assign(buffer, buffer + size);

        // Same masking problem as the size gate, one level up: an override that
        // is big enough but is not a usable part of this role (wrong build, wrong
        // shape, half-finished save) would fail the merge further down and push
        // the tile onto synthesis, whose output lacks everything only the retail
        // file carries. Prefer the CASC original, and report every outcome: a
        // fallback that fails quietly is indistinguishable from one that never
        // ran, which is exactly how a stale override kept masking retail once
        // already.
        std::string unusable_reason;
        if (!part_is_usable(result.bytes, part, &unusable_reason))
        {
          LogError << "[ModernADT][Save] The project override for '" << path << "' is "
                   << size << " byte(s) but is not a usable " << part_name(part)
                   << " part, because " << unusable_reason << ". Reading the original"
                      " from CASC instead." << std::endl;

          std::string how;
          auto casc_u8 = read_casc_original(path, client_data, &how);
          if (part_is_usable(casc_u8, part))
          {
            LogError << "[ModernADT][Save] Using the CASC original (" << casc_u8.size()
                     << " byte(s), read " << how << ") for '" << path << "' in place of"
                        " the stale override. Delete that file if the override was"
                        " not intentional." << std::endl;
            result.bytes = std::move(casc_u8);
          }
          else
          {
            LogError << "[ModernADT][Save] CASC has no usable " << part_name(part)
                     << " for '" << path << "' either, so the override stays; this"
                        " tile will synthesize if the merge refuses it." << std::endl;
          }
        }

        return result;
      }
      catch (...)
      {
        if (required)
          throw;
        return {};
      }
    }

    std::vector<ChunkView> parse_chunks(std::vector<std::uint8_t> const& bytes,
                                        std::size_t begin,
                                        std::size_t end,
                                        char const* label,
                                        bool legacy_mcnr_padding = false)
    {
      if (end > bytes.size() || begin > end)
        throw std::runtime_error(std::string(label) + " has an invalid chunk range.");

      std::vector<ChunkView> result;
      std::size_t pos = begin;
      while (pos + 8 <= end)
      {
        ChunkView chunk;
        chunk.offset = pos;
        chunk.payload = pos + 8;
        std::memcpy(&chunk.magic, bytes.data() + pos, 4);
        std::memcpy(&chunk.size, bytes.data() + pos + 4, 4);
        if (chunk.size > end - chunk.payload)
          throw std::runtime_error(std::string(label) + " contains a truncated chunk.");
        result.push_back(chunk);
        pos = chunk.payload + chunk.size;

        // Noggit's canonical WotLK serializer retains the 13 unknown bytes
        // following MCNR. They are padding, not another subchunk header.
        if (legacy_mcnr_padding && chunk.magic == fourcc('M','C','N','R'))
        {
          constexpr std::size_t mcnr_padding_size = 13;
          if (mcnr_padding_size > end - pos)
            throw std::runtime_error(std::string(label) + " has truncated MCNR padding.");
          pos += mcnr_padding_size;
        }
      }
      if (pos != end)
        throw std::runtime_error(std::string(label) + " does not end on a chunk boundary.");
      return result;
    }

    std::vector<ChunkView> parse_chunks(std::vector<std::uint8_t> const& bytes,
                                        char const* label)
    {
      return parse_chunks(bytes, 0, bytes.size(), label);
    }

    ChunkView const* find_chunk(std::vector<ChunkView> const& chunks, std::uint32_t magic)
    {
      auto const it = std::find_if(chunks.begin(), chunks.end(),
        [magic](ChunkView const& chunk) { return chunk.magic == magic; });
      return it == chunks.end() ? nullptr : &*it;
    }

    void append_range(std::vector<std::uint8_t>& output,
                      std::vector<std::uint8_t> const& input,
                      std::size_t begin,
                      std::size_t end)
    {
      output.insert(output.end(), input.begin() + begin, input.begin() + end);
    }

    std::vector<std::uint8_t> make_chunk(std::uint32_t magic,
                                         std::vector<std::uint8_t> const& payload)
    {
      if (payload.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("ADT chunk is too large to serialize.");
      std::vector<std::uint8_t> result(8 + payload.size());
      auto const size = static_cast<std::uint32_t>(payload.size());
      std::memcpy(result.data(), &magic, 4);
      std::memcpy(result.data() + 4, &size, 4);
      if (!payload.empty())
        std::memcpy(result.data() + 8, payload.data(), payload.size());
      return result;
    }

    std::vector<std::uint8_t> chunk_bytes(std::vector<std::uint8_t> const& bytes,
                                          ChunkView const& chunk)
    {
      return {bytes.begin() + chunk.offset, bytes.begin() + chunk.payload + chunk.size};
    }

    std::vector<std::uint8_t> rewrite_mcnk(
      std::vector<std::uint8_t> const& modern,
      ChunkView const& modern_chunk,
      std::vector<std::uint8_t> const& legacy,
      ChunkView const& legacy_chunk,
      bool root_part)
    {
      std::size_t const modern_header_size = root_part ? sizeof(MapChunkHeader) : 0;
      std::size_t const legacy_header_size = sizeof(MapChunkHeader);
      if (modern_chunk.size < modern_header_size || legacy_chunk.size < legacy_header_size)
        throw std::runtime_error("ADT MCNK header is truncated while saving.");

      auto const modern_sub_begin = modern_chunk.payload + modern_header_size;
      auto const legacy_sub_begin = legacy_chunk.payload + legacy_header_size;
      auto const modern_subs = parse_chunks(
        modern, modern_sub_begin, modern_chunk.payload + modern_chunk.size, "modern MCNK");
      auto const legacy_subs = parse_chunks(
        legacy, legacy_sub_begin, legacy_chunk.payload + legacy_chunk.size, "legacy MCNK", true);

      std::set<std::uint32_t> replaceable = root_part
        ? std::set<std::uint32_t>{fourcc('M','C','V','T'), fourcc('M','C','N','R'),
                                  fourcc('M','C','C','V'), fourcc('M','C','S','H'),
                                  fourcc('M','C','S','E')}
        : std::set<std::uint32_t>{fourcc('M','C','L','Y'), fourcc('M','C','A','L')};

      std::vector<std::uint8_t> payload;
      if (root_part)
      {
        append_range(payload, modern, modern_chunk.payload, modern_chunk.payload + modern_header_size);
        MapChunkHeader modern_header{};
        MapChunkHeader legacy_header{};
        std::memcpy(&modern_header, payload.data(), sizeof(modern_header));
        std::memcpy(&legacy_header, legacy.data() + legacy_chunk.payload, sizeof(legacy_header));
        // The same owner-split flag policy as ModernAdtWriter::
        // merge_chunk_header (see the bit-owner commentary there): bits 1-6
        // from the edited header, everything else from the retail original,
        // do_not_fix_alpha_map (bit 15, 0x8000) forced set. A cleared
        // do_not_fix next to full-size alpha/shadow data is the ERROR #134
        // "Bad genformat" crash.
        modern_header.flags.value =
          (legacy_header.flags.value & 0x7Eu)
          | (modern_header.flags.value & ~0x7Eu);
        modern_header.flags.flags.do_not_fix_alpha_map = 1;
        modern_header.ix = legacy_header.ix;
        modern_header.iy = legacy_header.iy;
        modern_header.areaid = legacy_header.areaid;
        if (modern_header.flags.flags.high_res_holes)
        {
          auto high_res = static_cast<std::uint64_t>(modern_header.ofsHeight)
                        | (static_cast<std::uint64_t>(modern_header.ofsNormal) << 32);
          auto const edited_low_res = legacy_header.holes & 0xFFFF;
          for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
            {
              auto const first = 2 * y * 8 + 2 * x;
              auto const block = (std::uint64_t{3} << first)
                               | (std::uint64_t{3} << (first + 8));
              auto const bit = 1u << (y * 4 + x);
              auto const was_hole = (high_res & block) != 0;
              auto const is_hole = (edited_low_res & bit) != 0;
              if (was_hole != is_hole)
                high_res = is_hole ? high_res | block : high_res & ~block;
            }
          modern_header.ofsHeight = static_cast<std::uint32_t>(high_res);
          modern_header.ofsNormal = static_cast<std::uint32_t>(high_res >> 32);
        }
        else
          modern_header.holes = (modern_header.holes & 0xFFFF0000u)
                              | (legacy_header.holes & 0xFFFFu);
        modern_header.ypos = legacy_header.ypos;
        std::copy(std::begin(legacy_header.doodadMapping), std::end(legacy_header.doodadMapping),
                  std::begin(modern_header.doodadMapping));
        std::copy(std::begin(legacy_header.doodadStencil), std::end(legacy_header.doodadStencil),
                  std::begin(modern_header.doodadStencil));
        std::memcpy(payload.data(), &modern_header, sizeof(modern_header));
      }

      std::set<std::uint32_t> emitted;
      for (auto const& current : modern_subs)
      {
        auto const* replacement = replaceable.count(current.magic)
          ? find_chunk(legacy_subs, current.magic) : nullptr;
        if (replacement)
        {
          append_range(payload, legacy, replacement->offset, replacement->payload + replacement->size);
          emitted.insert(current.magic);
        }
        else
        {
          append_range(payload, modern, current.offset, current.payload + current.size);
        }
      }

      for (auto const magic : replaceable)
      {
        if (emitted.count(magic))
          continue;
        if (auto const* replacement = find_chunk(legacy_subs, magic))
          append_range(payload, legacy, replacement->offset, replacement->payload + replacement->size);
      }
      return make_chunk(fourcc('M','C','N','K'), payload);
    }

    std::vector<std::uint8_t> rewrite_mcnks(
      std::vector<std::uint8_t> const& modern,
      std::vector<std::uint8_t> const& legacy,
      bool root_part)
    {
      auto const modern_chunks = parse_chunks(modern, root_part ? "modern ROOT" : "modern TEX0");
      auto const legacy_chunks = parse_chunks(legacy, "legacy ADT");
      std::vector<ChunkView const*> legacy_mcnks;
      for (auto const& chunk : legacy_chunks)
        if (chunk.magic == fourcc('M','C','N','K'))
          legacy_mcnks.push_back(&chunk);
      if (legacy_mcnks.size() != 256)
        throw std::runtime_error("Legacy serializer did not produce exactly 256 MCNK chunks.");

      std::vector<std::uint8_t> output;
      std::size_t mcnk_index = 0;
      for (auto const& chunk : modern_chunks)
      {
        if (chunk.magic != fourcc('M','C','N','K'))
        {
          append_range(output, modern, chunk.offset, chunk.payload + chunk.size);
          continue;
        }
        if (mcnk_index >= legacy_mcnks.size())
          throw std::runtime_error("Modern split ADT contains too many MCNK chunks.");
        auto rewritten = rewrite_mcnk(
          modern, chunk, legacy, *legacy_mcnks[mcnk_index++], root_part);
        output.insert(output.end(), rewritten.begin(), rewritten.end());
      }
      if (mcnk_index != 256)
        throw std::runtime_error("Modern split ADT does not contain exactly 256 MCNK chunks.");
      return output;
    }

    std::vector<std::uint8_t> rewrite_obj0_refs(
      std::vector<std::uint8_t> const& obj0,
      std::vector<std::uint8_t> const& legacy,
      std::vector<std::int32_t> const& doodad_remap,
      std::vector<std::int32_t> const& wmo_remap)
    {
      auto const modern_chunks = parse_chunks(obj0, "modern OBJ0");
      auto const legacy_chunks = parse_chunks(legacy, "legacy ADT");
      std::vector<ChunkView const*> legacy_mcnks;
      for (auto const& chunk : legacy_chunks)
        if (chunk.magic == fourcc('M','C','N','K'))
          legacy_mcnks.push_back(&chunk);
      if (legacy_mcnks.size() != 256)
        throw std::runtime_error("Legacy ADT does not contain exactly 256 MCNK chunks for OBJ0 references.");

      std::vector<std::uint8_t> output;
      std::size_t mcnk_index = 0;
      for (auto const& chunk : modern_chunks)
      {
        if (chunk.magic != fourcc('M','C','N','K'))
        {
          append_range(output, obj0, chunk.offset, chunk.payload + chunk.size);
          continue;
        }
        if (mcnk_index >= legacy_mcnks.size())
          throw std::runtime_error("Modern OBJ0 contains too many MCNK chunks.");

        auto const& source = *legacy_mcnks[mcnk_index++];
        if (source.size < sizeof(MapChunkHeader))
          throw std::runtime_error("Legacy MCNK header is truncated while rebuilding OBJ0 references.");
        MapChunkHeader header{};
        std::memcpy(&header, legacy.data() + source.payload, sizeof(header));
        auto const legacy_subs = parse_chunks(
          legacy, source.payload + sizeof(header), source.payload + source.size,
          "legacy MCNK", true);
        auto const* refs = find_chunk(legacy_subs, fourcc('M','C','R','F'));
        auto const doodad_bytes = static_cast<std::size_t>(header.nDoodadRefs) * 4;
        auto const wmo_bytes = static_cast<std::size_t>(header.nMapObjRefs) * 4;
        if (!refs || refs->size != doodad_bytes + wmo_bytes)
          throw std::runtime_error("Legacy MCNK MCRF size does not match object reference counts.");

        // MCRF holds nDoodadRefs indices into MDDF followed by nMapObjRefs
        // indices into MODF. Both lists were just compacted, because a placement
        // whose asset cannot be resolved at all is dropped rather than costing
        // the whole tile. Every index after a drop has therefore moved, so the
        // references are renumbered through the tables and the ones whose
        // placement is gone are dropped with it. Validating them against the new
        // counts instead - which is what this used to do - threw as soon as a
        // single object was unresolvable and lost the tile anyway.
        auto const remap_refs = [&legacy](std::size_t at, std::uint32_t count,
                                          std::vector<std::int32_t> const& table)
        {
          std::vector<std::uint8_t> out;
          out.reserve(static_cast<std::size_t>(count) * 4);
          for (std::uint32_t i = 0; i < count; ++i)
          {
            std::uint32_t index = 0;
            std::memcpy(&index, legacy.data() + at + static_cast<std::size_t>(i) * 4, 4);
            if (index >= table.size())
              throw std::runtime_error("Legacy MCNK references an invalid OBJ0 placement index.");
            if (table[index] < 0)
              continue;
            std::uint32_t const mapped = static_cast<std::uint32_t>(table[index]);
            out.insert(out.end(), reinterpret_cast<std::uint8_t const*>(&mapped),
                       reinterpret_cast<std::uint8_t const*>(&mapped) + 4);
          }
          return out;
        };

        auto const doodad_refs = remap_refs(refs->payload, header.nDoodadRefs, doodad_remap);
        auto const wmo_refs = remap_refs(refs->payload + doodad_bytes, header.nMapObjRefs,
                                         wmo_remap);
        auto const modern_subs = parse_chunks(
          obj0, chunk.payload, chunk.payload + chunk.size, "modern OBJ0 MCNK");
        std::vector<std::uint8_t> payload;
        bool emitted_doodads = false;
        bool emitted_wmos = false;
        for (auto const& current : modern_subs)
        {
          if (current.magic == fourcc('M','C','R','D'))
          {
            if (emitted_doodads)
              throw std::runtime_error("Modern OBJ0 MCNK has duplicate MCRD chunks.");
            auto rewritten = make_chunk(current.magic, doodad_refs);
            payload.insert(payload.end(), rewritten.begin(), rewritten.end());
            emitted_doodads = true;
          }
          else if (current.magic == fourcc('M','C','R','W'))
          {
            if (emitted_wmos)
              throw std::runtime_error("Modern OBJ0 MCNK has duplicate MCRW chunks.");
            auto rewritten = make_chunk(current.magic, wmo_refs);
            payload.insert(payload.end(), rewritten.begin(), rewritten.end());
            emitted_wmos = true;
          }
          else
            append_range(payload, obj0, current.offset, current.payload + current.size);
        }
        if (!emitted_doodads && !doodad_refs.empty())
        {
          auto rewritten = make_chunk(fourcc('M','C','R','D'), doodad_refs);
          payload.insert(payload.end(), rewritten.begin(), rewritten.end());
        }
        if (!emitted_wmos && !wmo_refs.empty())
        {
          auto rewritten = make_chunk(fourcc('M','C','R','W'), wmo_refs);
          payload.insert(payload.end(), rewritten.begin(), rewritten.end());
        }
        auto rewritten = make_chunk(fourcc('M','C','N','K'), payload);
        output.insert(output.end(), rewritten.begin(), rewritten.end());
      }
      if (mcnk_index != 256)
        throw std::runtime_error("Modern OBJ0 does not contain exactly 256 MCNK chunks.");
      return output;
    }

    std::vector<std::string> legacy_string_table(std::vector<std::uint8_t> const& legacy,
                                                  std::uint32_t magic)
    {
      auto const chunks = parse_chunks(legacy, "legacy ADT");
      auto const* chunk = find_chunk(chunks, magic);
      if (!chunk)
        return {};
      std::vector<std::string> result;
      std::size_t pos = chunk->payload;
      auto const end = chunk->payload + chunk->size;
      while (pos < end)
      {
        auto const* value = reinterpret_cast<char const*>(legacy.data() + pos);
        auto const remaining = end - pos;
        auto const length = strnlen(value, remaining);
        if (length == remaining)
          throw std::runtime_error("Legacy ADT contains an unterminated string table entry.");
        result.emplace_back(value, length);
        pos += length + 1;
      }
      return result;
    }

    //! Resolve an asset path to a FileDataID.
    //!
    //! The editor's own hints come first: they carry the id the asset was really
    //! loaded with, which the project listfile does not necessarily know. Retail
    //! installs routinely have assets whose id Noggit had to guess (they get a
    //! synthetic path such as models/world/unk_exp11_6212428/6212428.m2) and
    //! partial CASC copies are missing listfile entries altogether.
    std::uint32_t lookup_fdid(std::string const& path, ModernAdtFdidHints const& hints)
    {
      auto const normalized = BlizzardArchive::ClientData::normalizeFilenameInternal(path);

      if (auto const it = hints.find(normalized); it != hints.end() && it->second)
        return it->second;

      return Noggit::Application::NoggitApplication::instance()->clientData()
               ->listfile()->getFileDataID(normalized);
    }

    //! Resolve the MTEX string table into an MDID payload.
    //!
    //! MDID is index aligned with MCLY, so an unresolvable layer has to keep its
    //! slot: id 0 leaves that one layer untextured. Throwing here instead used to
    //! abort the whole tile conversion before anything was written, so a single
    //! texture the listfile did not know cost the player every height, texture,
    //! object and water edit on that tile.
    std::vector<std::uint32_t> resolve_file_ids(std::vector<std::string> const& paths,
                                                ModernAdtFdidHints const& fdid_hints,
                                                ModernAdtSaveReport* report)
    {
      std::vector<std::uint32_t> result;
      result.reserve(paths.size());
      for (auto const& path : paths)
      {
        auto const id = lookup_fdid(path, fdid_hints);
        if (!id && report)
        {
          ++report->unresolved_textures;
          if (report->first_unresolved_texture.empty())
            report->first_unresolved_texture = path;
        }
        result.push_back(id);
      }
      return result;
    }

    std::vector<std::uint8_t> uint32_payload(std::vector<std::uint32_t> const& values)
    {
      std::vector<std::uint8_t> result(values.size() * sizeof(std::uint32_t));
      if (!result.empty())
        std::memcpy(result.data(), values.data(), result.size());
      return result;
    }

    std::vector<std::uint8_t> replace_top_level_chunk(
      std::vector<std::uint8_t> const& input,
      std::uint32_t magic,
      std::vector<std::uint8_t> const& replacement)
    {
      auto const chunks = parse_chunks(input, "split ADT");
      std::vector<std::uint8_t> output;
      bool replaced = false;
      for (auto const& chunk : chunks)
      {
        if (chunk.magic == magic)
        {
          if (!replaced)
            output.insert(output.end(), replacement.begin(), replacement.end());
          replaced = true;
        }
        else
          append_range(output, input, chunk.offset, chunk.payload + chunk.size);
      }
      if (!replaced)
        output.insert(output.end(), replacement.begin(), replacement.end());
      return output;
    }

    void validate_tex0_alpha_layout(std::vector<std::uint8_t> const& tex0)
    {
      auto const chunks = parse_chunks(tex0, "saved TEX0");
      auto const* mdid = find_chunk(chunks, fourcc('M','D','I','D'));
      if (!mdid || mdid->size % sizeof(std::uint32_t) != 0)
        throw std::runtime_error("Saved TEX0 has an invalid MDID chunk.");
      auto const texture_count = mdid->size / sizeof(std::uint32_t);

      std::size_t mcnk_count = 0;
      for (auto const& chunk : chunks)
      {
        if (chunk.magic != fourcc('M','C','N','K'))
          continue;
        ++mcnk_count;

        auto const subchunks = parse_chunks(
          tex0, chunk.payload, chunk.payload + chunk.size, "saved TEX0 MCNK");
        auto const* mcly = find_chunk(subchunks, fourcc('M','C','L','Y'));
        auto const* mcal = find_chunk(subchunks, fourcc('M','C','A','L'));
        if (!mcly || mcly->size % sizeof(ENTRY_MCLY) != 0)
          throw std::runtime_error("Saved TEX0 MCNK has an invalid MCLY chunk.");

        auto const layer_count = mcly->size / sizeof(ENTRY_MCLY);
        std::vector<ENTRY_MCLY> layers(layer_count);
        if (mcly->size)
          std::memcpy(layers.data(), tex0.data() + mcly->payload, mcly->size);

        for (std::size_t layer_index = 0; layer_index < layers.size(); ++layer_index)
        {
          auto const& layer = layers[layer_index];
          if (layer.textureID >= texture_count)
            throw std::runtime_error("Saved TEX0 MCLY references a texture outside MDID.");
          if (layer_index == 0 || !(layer.flags & FLAG_USE_ALPHA))
            continue;
          if (!mcal || layer.ofsAlpha > mcal->size)
            throw std::runtime_error("Saved TEX0 MCLY references an invalid MCAL offset.");

          auto alpha_end = mcal->size;
          for (std::size_t next = layer_index + 1; next < layers.size(); ++next)
          {
            if ((layers[next].flags & FLAG_USE_ALPHA)
                && layers[next].ofsAlpha >= layer.ofsAlpha)
            {
              alpha_end = layers[next].ofsAlpha;
              break;
            }
          }
          if (alpha_end < layer.ofsAlpha)
            throw std::runtime_error("Saved TEX0 alpha maps overlap.");

          auto const alpha_size = alpha_end - layer.ofsAlpha;
          if (!(layer.flags & FLAG_ALPHA_COMPRESSED) && alpha_size != 4096)
          {
            std::ostringstream message;
            message << "Saved TEX0 has an uncompressed alpha map with "
                    << alpha_size << " bytes instead of 4096.";
            throw std::runtime_error(message.str());
          }
        }
      }
      if (mcnk_count != 256)
        throw std::runtime_error("Saved TEX0 does not contain exactly 256 MCNK chunks.");
    }

    std::string indexed_path(std::vector<std::uint8_t> const& legacy,
                             ChunkView const* offsets_chunk,
                             ChunkView const* strings_chunk,
                             std::uint32_t index)
    {
      if (!offsets_chunk || !strings_chunk || index >= offsets_chunk->size / 4)
        throw std::runtime_error("Legacy ADT placement references an invalid filename index.");
      std::uint32_t offset = 0;
      std::memcpy(&offset, legacy.data() + offsets_chunk->payload + index * 4, 4);
      if (offset >= strings_chunk->size)
        throw std::runtime_error("Legacy ADT placement references an invalid filename offset.");
      auto const* value = reinterpret_cast<char const*>(legacy.data() + strings_chunk->payload + offset);
      auto const remaining = strings_chunk->size - offset;
      auto const length = strnlen(value, remaining);
      if (length == remaining)
        throw std::runtime_error("Legacy ADT placement filename is unterminated.");
      return {value, length};
    }

    template<typename Entry>
    std::vector<std::uint8_t> modern_placements(std::vector<std::uint8_t> const& legacy,
                                                std::uint32_t placement_magic,
                                                std::uint32_t offsets_magic,
                                                std::uint32_t strings_magic,
                                                std::vector<std::uint8_t> const& original_obj0,
                                                ModernAdtFdidHints const& fdid_hints,
                                                ModernAdtSaveReport* report,
                                                std::vector<std::int32_t>& index_remap)
    {
      auto const chunks = parse_chunks(legacy, "legacy ADT");
      auto const* placements = find_chunk(chunks, placement_magic);
      if (!placements)
        return {};
      if (placements->size % sizeof(Entry) != 0)
        throw std::runtime_error("Legacy ADT placement chunk has an invalid size.");
      auto const* offsets = find_chunk(chunks, offsets_magic);
      auto const* strings = find_chunk(chunks, strings_magic);
      auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();

      std::unordered_map<std::uint64_t, std::uint16_t> original_flags;
      //! In a modern OBJ0 the nameID *is* the FileDataID, so a placement that was
      //! loaded from this tile can always give its id back even when its path
      //! cannot be resolved any more.
      std::unordered_map<std::uint32_t, std::uint32_t> original_fdid_by_uid;
      {
        auto const original_chunks = parse_chunks(original_obj0, "original OBJ0");
        if (auto const* original = find_chunk(original_chunks, placement_magic))
        {
          if (original->size % sizeof(Entry) != 0)
            throw std::runtime_error("Original OBJ0 placement chunk has an invalid size.");
          for (std::size_t i = 0; i < original->size / sizeof(Entry); ++i)
          {
            Entry entry{};
            std::memcpy(&entry, original_obj0.data() + original->payload + i * sizeof(Entry), sizeof(Entry));
            original_fdid_by_uid.emplace(entry.uniqueID, entry.nameID);

            if constexpr (std::is_same_v<Entry, ENTRY_MDDF>)
            {
              auto const key = (static_cast<std::uint64_t>(entry.uniqueID) << 32) | entry.nameID;
              original_flags.emplace(key, entry.flags);
            }
          }
        }
      }

      std::vector<Entry> kept;
      kept.reserve(placements->size / sizeof(Entry));

      for (std::size_t i = 0; i < placements->size / sizeof(Entry); ++i)
      {
        Entry entry{};
        std::memcpy(&entry, legacy.data() + placements->payload + i * sizeof(Entry), sizeof(Entry));
        auto const path = indexed_path(legacy, offsets, strings, entry.nameID);
        auto id = lookup_fdid(path, fdid_hints);

        if (!id)
        {
          // Unmodified placements keep the id they were loaded with, so an
          // unresolvable path on an existing object costs nothing.
          if (auto const it = original_fdid_by_uid.find(entry.uniqueID);
              it != original_fdid_by_uid.end() && it->second)
          {
            id = it->second;
            if (report)
            {
              ++report->recovered_placements;
              if (report->first_recovered_placement.empty())
                report->first_recovered_placement = path;
            }
          }
        }

        if (!id)
        {
          // Drop this one placement rather than the tile. A newly placed object
          // whose asset is not in the listfile cannot be written as a FileDataID,
          // but aborting here lost every other edit on the tile as well.
          if (report)
          {
            ++report->skipped_placements;
            if (report->first_skipped_placement.empty())
              report->first_skipped_placement = path;
          }
          index_remap.push_back(-1);
          continue;
        }

        entry.nameID = id;
        if constexpr (std::is_same_v<Entry, ENTRY_MDDF>)
        {
          auto const key = (static_cast<std::uint64_t>(entry.uniqueID) << 32) | entry.nameID;
          if (auto const it = original_flags.find(key); it != original_flags.end())
            entry.flags = it->second;
          // Retail OBJ0 stores FileDataIDs without this flag; Shadowlands uses it.
          if (client_data->version() == BlizzardArchive::ClientVersion::RETAIL)
            entry.flags &= ~static_cast<std::uint16_t>(0x40);
          else
            entry.flags |= 0x40;
        }
        else if constexpr (std::is_same_v<Entry, ENTRY_MODF>)
        {
          // Existing modern OBJ0 WMO placements use these bits with FileDataIDs.
          entry.flags |= 0x000C;
          Conversion::sanitizeModfEntry(entry);
        }
        // Record the new index before appending, so the per-chunk MCRF
        // references can be renumbered: dropping a placement shifts every index
        // after it, and the references are indices into this compacted list.
        index_remap.push_back(static_cast<std::int32_t>(kept.size()));
        kept.push_back(entry);
      }

      std::vector<std::uint8_t> payload(kept.size() * sizeof(Entry));
      if (!kept.empty())
        std::memcpy(payload.data(), kept.data(), payload.size());
      return payload;
    }

    void save_part(std::string const& logical_path, std::vector<std::uint8_t> const& bytes)
    {
      auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
      BlizzardArchive::Listfile::FileKey key(logical_path);
      auto const disk_path = client_data->getDiskPath(key);
      std::filesystem::create_directories(std::filesystem::path(disk_path).parent_path());

      QSaveFile file(QString::fromStdString(disk_path));
      if (!file.open(QIODevice::WriteOnly)
          || file.write(reinterpret_cast<char const*>(bytes.data()),
                        static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size())
          || !file.commit())
        throw std::runtime_error("Could not atomically save modern ADT part: " + disk_path);
    }

    // Shared save pipeline for both the native and the conversion path:
    // assemble every part in memory, validate everything, then write.
    struct ConvertedParts
    {
      std::vector<std::uint8_t> root;
      std::vector<std::uint8_t> tex0;
      std::vector<std::uint8_t> obj0;
      std::vector<std::uint8_t> obj1;
      //! OBJ1 and LOD are written from the originals only when the rest of the
      //! tile was merged into them. Synthesized parts do not line up with the
      //! original OBJ1 any more, and an OBJ1 that contradicts its ROOT is worse
      //! than none: the loader treats a missing optional part as a degraded
      //! load, not as a refusal. Whatever is not overridden still comes from
      //! CASC untouched.
      bool write_obj1 = false;
      bool write_lod = false;
    };

    //! Base name of the map a ROOT path belongs to, for conversion metadata.
    std::string map_name_from_root_path(std::string const& root_path)
    {
      std::filesystem::path const path(root_path);
      auto const parent = path.parent_path();
      return parent.empty() ? path.stem().string() : parent.filename().string();
    }

    //! Whether the originals in this backing store can be merged into.
    //!
    //! The normal save patches the original split tile so every chunk the editor
    //! does not own survives. That needs a usable ROOT (MVER/MHDR/MCIN plus the
    //! 256 MCNKs) and TEX0/OBJ0 to rewrite. When any of them is missing, or too
    //! small to hold a chunk header, merging is impossible - which used to abort
    //! the save and cost the whole tile.
    //!
    //! The three are decided together on purpose. A synthesized TEX0 written next
    //! to an original-derived ROOT would not be self-consistent: MDID indices in
    //! one have to line up with MCLY layers in the other.
    bool backing_supports_merge(ShadowlandsADTBackingStore const& backing,
                                std::string* reason)
    {
      auto const fail = [reason](std::string text)
      {
        if (reason)
          *reason = std::move(text);
        return false;
      };

      struct required_part
      {
        ShadowlandsADTPart part;
        char const* label;
      };

      for (auto const& entry : {required_part{ShadowlandsADTPart::Root, "ROOT"},
                                required_part{ShadowlandsADTPart::Tex0, "TEX0"},
                                required_part{ShadowlandsADTPart::Obj0, "OBJ0"}})
      {
        if (!backing.has(entry.part))
          return fail(std::string(entry.label) + " could not be read");

        auto const size = backing.part(entry.part).bytes.size();
        if (size < minimum_adt_part_size)
          return fail(std::string(entry.label) + " is " + std::to_string(size)
                      + " byte(s), too small to hold a chunk header");
      }

      try
      {
        auto const chunks =
          parse_chunks(backing.part(ShadowlandsADTPart::Root).bytes, "original ROOT");

        // MCIN is not in this list: the native writer synthesizes one when
        // the retail original omits it (see writeRoot).
        for (auto const magic : {fourcc('M','V','E','R'), fourcc('M','H','D','R'),
                                 fourcc('M','C','N','K')})
        {
          if (!find_chunk(chunks, magic))
            return fail("original ROOT is missing a chunk the writer needs");
        }
      }
      catch (std::exception const& error)
      {
        return fail(std::string("original ROOT does not parse: ") + error.what());
      }

      return true;
    }

    //! Build the split parts from the edited tile alone, in the client's own
    //! modern format.
    //!
    //! This is the recovery path for a tile whose originals cannot be merged
    //! into. It reuses the legacy-to-modern converter, so the output is the same
    //! ROOT/TEX0/OBJ0 a converted map gets rather than a downgraded WotLK file:
    //! a modern client and Noggit's split-ADT gate both read it as a modern
    //! split tile. What cannot be recovered is data Noggit never had - chunks
    //! with no legacy equivalent are simply absent.
    ConvertedParts synthesize_parts(ShadowlandsADTBackingStore const& backing,
                                    std::vector<std::uint8_t> const& legacy_adt,
                                    ModernAdtFdidHints const& fdid_hints,
                                    ModernAdtSaveReport* report)
    {
      auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();

      Conversion::ConversionFileSource source;
      // Nothing else is read: a single tile is converted from bytes already in
      // hand, not from files on disk.
      source.read_file = [](std::string const&)
      {
        return std::optional<std::vector<std::uint8_t>>{};
      };
      // Same resolution order the merge path uses: the FileDataID the tile was
      // loaded with, then the project listfile. Feeding the hints in is what
      // keeps assets resolving on a retail install whose listfile is incomplete;
      // without them the allocator would invent ids the client never heard of.
      source.path_to_fdid = [&fdid_hints](std::string const& path)
      {
        if (auto const id = lookup_fdid(path, fdid_hints))
          return std::optional<std::uint32_t>{id};
        return std::optional<std::uint32_t>{};
      };
      source.fdid_to_path = [client_data](std::uint32_t fdid)
      {
        return client_data->listfile()->getPath(fdid);
      };

      Conversion::ConversionOptions options;
      options.target = Conversion::MapFormat::modern;
      options.map_name = map_name_from_root_path(backing.source.root_path);
      options.convert_liquids = true;
      options.include_wdl = false;

      Conversion::FdidAllocator fdids(options.custom_fdid_base);
      Conversion::AdtConversionResult result;
      try
      {
        result = Conversion::convertAdtLegacyToModern(legacy_adt, options, source, fdids);
      }
      catch (std::exception const& error)
      {
        throw ModernAdtWriterError(std::string("could not synthesize modern split parts: ")
                                   + error.what());
      }

      if (!result.ok || result.root.empty() || result.tex0.empty() || result.obj0.empty())
      {
        std::string first_error;
        for (auto const& issue : result.issues)
        {
          if (issue.severity == Conversion::ConversionIssue::Severity::error)
          {
            first_error = issue.message;
            break;
          }
        }
        throw ModernAdtWriterError("could not synthesize modern split parts from the edited"
                                   " tile" + (first_error.empty() ? std::string()
                                                                  : ": " + first_error));
      }

      // Ids the allocator had to invent are still written, but the client has no
      // such FileDataID, so those assets stay invisible. That is worth saying out
      // loud rather than leaving the player to wonder.
      if (report)
      {
        report->synthesized = true;
        for (auto const& assignment : fdids.assignments())
        {
          if (assignment.existed_in_client)
            continue;
          ++report->custom_fdids;
          if (report->first_custom_fdid.empty())
            report->first_custom_fdid = assignment.path;
        }
      }

      ConvertedParts parts;
      parts.root = std::move(result.root);
      parts.tex0 = std::move(result.tex0);
      parts.obj0 = std::move(result.obj0);

      // _obj1 and _lod have no legacy equivalent to re-derive from: OBJ1 holds
      // the LOD object placements and LOD is the low resolution tile. The merge
      // path rewrites OBJ1 against its new OBJ0 indices, but synthesis has no
      // original OBJ0 to diff against, so pass both through untouched. They can
      // then be stale relative to the edited tile - a LOD object may sit where
      // it was before the edit - but a stale companion beats a missing one: the
      // client expects the full per-tile file set, and dropping these leaves a
      // hole in it with no LOD objects at all.
      parts.write_obj1 = backing.has(ShadowlandsADTPart::Obj1);
      if (parts.write_obj1)
        parts.obj1 = backing.part(ShadowlandsADTPart::Obj1).bytes;
      parts.write_lod = backing.has(ShadowlandsADTPart::Lod);
      return parts;
    }

    ConvertedParts assemble_parts(ShadowlandsADTBackingStore const& backing,
                                  std::vector<std::uint8_t> const& legacy_adt,
                                  std::optional<std::vector<std::uint8_t>> native_root,
                                  ModernAdtFdidHints const& fdid_hints,
                                  ModernAdtSaveReport* report)
    {
      std::string merge_blocker;
      if (!backing_supports_merge(backing, &merge_blocker))
      {
        LogError << "[ModernADT][Save] The original split tile for '"
                 << backing.source.root_path << "' cannot be merged into ("
                 << merge_blocker << "), so the modern parts were synthesized from"
                    " the edited tile instead of the save being abandoned. The"
                    " output is still a modern split ADT." << std::endl;
        return synthesize_parts(backing, legacy_adt, fdid_hints, report);
      }

      ConvertedParts parts;
      auto const& profile = adt_profile_v18();

      if (native_root)
      {
        parts.root = std::move(*native_root);
      }
      else
      {
        parts.root = rewrite_mcnks(backing.part(ShadowlandsADTPart::Root).bytes, legacy_adt, true);
        auto const legacy_chunks = parse_chunks(legacy_adt, "legacy ADT");
        for (auto const magic : {fourcc('M','H','2','O'), fourcc('M','F','B','O')})
        {
          if (auto const* replacement = find_chunk(legacy_chunks, magic))
            parts.root = replace_top_level_chunk(parts.root, magic, chunk_bytes(legacy_adt, *replacement));
        }
        // Chunk replacements shift file positions while rewrite_mcnks keeps the
        // original MCIN/MHDR, which leaves their offsets stale. Refresh the
        // bookkeeping against the actual layout before validating.
        ModernAdtWriter::refreshRootBookkeeping(profile, parts.root);
      }

      parts.tex0 = rewrite_mcnks(backing.part(ShadowlandsADTPart::Tex0).bytes, legacy_adt, false);
      auto const texture_paths = legacy_string_table(legacy_adt, fourcc('M','T','E','X'));
      auto const texture_ids = resolve_file_ids(texture_paths, fdid_hints, report);
      parts.tex0 = replace_top_level_chunk(
        parts.tex0, fourcc('M','D','I','D'), make_chunk(fourcc('M','D','I','D'), uint32_payload(texture_ids)));

      auto const original_obj0 = backing.part(ShadowlandsADTPart::Obj0).bytes;
      std::vector<std::int32_t> doodad_remap;
      std::vector<std::int32_t> wmo_remap;
      auto const mddf = modern_placements<ENTRY_MDDF>(
        legacy_adt, fourcc('M','D','D','F'), fourcc('M','M','I','D'), fourcc('M','M','D','X'),
        original_obj0, fdid_hints, report, doodad_remap);
      auto const modf = modern_placements<ENTRY_MODF>(
        legacy_adt, fourcc('M','O','D','F'), fourcc('M','W','I','D'), fourcc('M','W','M','O'),
        original_obj0, fdid_hints, report, wmo_remap);
      parts.write_obj1 = backing.has(ShadowlandsADTPart::Obj1);
      parts.write_lod = backing.has(ShadowlandsADTPart::Lod);
      if (parts.write_obj1)
      {
        // The LOD placement regeneration lives in the Qt-free writer so the
        // standalone format harness pins the retail layout variants down.
        // Retail tiles carry the doodad side (MLDD/MLDX[/MLDL/MDLI/MLDB])
        // and the WMO side (MLMD/MLMX[/MLMB/MWDR]) independently; requiring
        // every chunk at once refused the merge for 169 azeroth tiles in one
        // session and left all of their edits unwritten.
        auto lod = rewriteObj1Lod(backing.part(ShadowlandsADTPart::Obj1).bytes, mddf, modf);
        if (!lod.reason.empty())
          LogError << "[ModernADT][Save] OBJ1 of '" << backing.source.root_path << "': "
                   << lod.reason << ". The rest of the tile saved normally; its"
                      " distant LOD objects may lag the edits (visual only)."
                   << std::endl;
        parts.obj1 = std::move(lod.bytes);
      }
      parts.obj0 = original_obj0;
      parts.obj0 = replace_top_level_chunk(
        parts.obj0, fourcc('M','D','D','F'), make_chunk(fourcc('M','D','D','F'), mddf));
      parts.obj0 = replace_top_level_chunk(
        parts.obj0, fourcc('M','O','D','F'), make_chunk(fourcc('M','O','D','F'), modf));
      parts.obj0 = rewrite_obj0_refs(parts.obj0, legacy_adt, doodad_remap, wmo_remap);
      {
        // MODF was just rebuilt, so the per-WMO companion chunks (MLMB class
        // bytes, MWDR/MWDS doodad-set ranges) have to follow it: copied
        // byte-for-byte they disagree with the placements they annotate, and
        // a count the client walks past is an out-of-bounds read while the
        // tile streams in.
        auto companions = rewriteObj0Companions(parts.obj0, original_obj0);
        parts.obj0 = std::move(companions.bytes);
        if (!companions.note.empty())
          LogError << "[ModernADT][Save] OBJ0 of '" << backing.source.root_path << "': "
                   << companions.note << "." << std::endl;
        if (report && companions.defaults_used)
        {
          report->companion_defaults += companions.defaults_used;
          if (!report->first_companion_default_fdid)
            report->first_companion_default_fdid = companions.first_default_fdid;
        }
      }
      return parts;
    }

    void validate_and_save_parts(ShadowlandsADTBackingStore const& backing,
                                 ConvertedParts const& parts)
    {
      auto const& profile = adt_profile_v18();

      // Pre-write validation (issue: native modern ADT writer). A part that
      // fails here never reaches disk; ModernAdtValidationError tells callers
      // that falling back to another path would write the same suspect data.
      ModernAdtWriter::validatePartOrThrow(profile, parts.root, AdtPartExpectation::Root, "ROOT part");
      ModernAdtWriter::validatePartOrThrow(profile, parts.tex0, AdtPartExpectation::Texture, "TEX0 part");
      try
      {
        validate_tex0_alpha_layout(parts.tex0);
      }
      catch (ModernAdtValidationError const&)
      {
        throw;
      }
      catch (std::exception const& error)
      {
        throw ModernAdtValidationError(std::string("TEX0 alpha layout: ") + error.what());
      }
      ModernAdtWriter::validatePartOrThrow(profile, parts.obj0, AdtPartExpectation::Object, "OBJ0 part");
      if (parts.write_obj1)
        ModernAdtWriter::validatePartOrThrow(profile, parts.obj1, AdtPartExpectation::Generic, "OBJ1 part");
      if (parts.write_lod)
        ModernAdtWriter::validatePartOrThrow(
          profile, backing.part(ShadowlandsADTPart::Lod).bytes, AdtPartExpectation::Generic, "LOD part");

      // Build every part before touching disk. QSaveFile then replaces each loose
      // project override atomically and the original CASC remains read-only.
      save_part(backing.source.paths[part_index(ShadowlandsADTPart::Root)], parts.root);
      save_part(backing.source.paths[part_index(ShadowlandsADTPart::Tex0)], parts.tex0);
      save_part(backing.source.paths[part_index(ShadowlandsADTPart::Obj0)], parts.obj0);
      if (parts.write_obj1)
        save_part(backing.source.paths[part_index(ShadowlandsADTPart::Obj1)], parts.obj1);
      if (parts.write_lod)
        save_part(backing.source.paths[part_index(ShadowlandsADTPart::Lod)],
                  backing.part(ShadowlandsADTPart::Lod).bytes);

      // Name what actually reached disk. Claiming parts that were skipped makes
      // the log contradict the project folder, which is what gets checked
      // against it.
      std::string skipped;
      if (!parts.write_obj1)
        skipped += "_obj1";
      if (!parts.write_lod)
      {
        if (!skipped.empty())
          skipped += " and ";
        skipped += "_lod";
      }

      Log << "[ModernADT][Save] Saved split tile ROOT/TEX0/OBJ0"
          << (parts.write_obj1 ? "/OBJ1" : "")
          << (parts.write_lod ? "/LOD" : "")
          << " to the project override: " << backing.source.root_path
          << (skipped.empty()
                ? std::string()
                : " (the original tile had no " + skipped + " to carry over, so "
                  + (skipped.find(" and ") == std::string::npos ? "it was" : "they were")
                  + " not written)")
          << std::endl;
    }
  }

  bool ShadowlandsADTBackingStore::has(ShadowlandsADTPart part) const noexcept
  {
    return parts[part_index(part)].present;
  }

  ShadowlandsADTRawPart const& ShadowlandsADTBackingStore::part(ShadowlandsADTPart part) const noexcept
  {
    return parts[part_index(part)];
  }

  ShadowlandsADTRawPart& ShadowlandsADTBackingStore::part(ShadowlandsADTPart part) noexcept
  {
    return parts[part_index(part)];
  }

  ShadowlandsADTSource ShadowlandsADTCodec::makeSource(std::string const& root_path)
  {
    ShadowlandsADTSource source;
    source.root_path = root_path;
    source.paths[part_index(ShadowlandsADTPart::Root)] = root_path;
    source.paths[part_index(ShadowlandsADTPart::Tex0)] = split_path(root_path, "_tex0");
    source.paths[part_index(ShadowlandsADTPart::Obj0)] = split_path(root_path, "_obj0");
    source.paths[part_index(ShadowlandsADTPart::Obj1)] = split_path(root_path, "_obj1");
    source.paths[part_index(ShadowlandsADTPart::Lod)] = split_path(root_path, "_lod");
    return source;
  }

  ShadowlandsADTBackingStore ShadowlandsADTCodec::loadBackingStore(std::string const& root_path)
  {
    ShadowlandsADTBackingStore backing;
    backing.source = makeSource(root_path);

    backing.parts[part_index(ShadowlandsADTPart::Root)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Root)], true,
                ShadowlandsADTPart::Root);
    backing.parts[part_index(ShadowlandsADTPart::Tex0)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Tex0)], true,
                ShadowlandsADTPart::Tex0);
    backing.parts[part_index(ShadowlandsADTPart::Obj0)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Obj0)], true,
                ShadowlandsADTPart::Obj0);

    // These are not required by every map/tile.  They are nevertheless loaded
    // when available so future native writers can preserve their contents.
    backing.parts[part_index(ShadowlandsADTPart::Obj1)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Obj1)], false,
                ShadowlandsADTPart::Obj1);
    backing.parts[part_index(ShadowlandsADTPart::Lod)] =
      read_part(backing.source.paths[part_index(ShadowlandsADTPart::Lod)], false,
                ShadowlandsADTPart::Lod);

    // Sizes, not contents: when a part turns out to be unusable further down,
    // "0 bytes" and "the wrong file" need to be tellable apart in the log.
    auto part_size = [&backing] (ShadowlandsADTPart part)
    {
      return backing.has(part)
               ? std::to_string(backing.part(part).bytes.size())
               : std::string("absent");
    };

    //! The first four bytes of a part as text, straight from the file - no
    //! fourcc byte-order assumption, so it cannot misreport a valid part. An ADT
    //! part starts with its version chunk, so anything else here is the answer to
    //! "why did the writer say this ROOT has no MVER".
    auto first_bytes = [] (std::vector<std::uint8_t> const& bytes)
    {
      if (bytes.size() < 4)
        return std::string("(none)");

      std::string text;
      for (std::size_t i = 0; i < 4; ++i)
      {
        auto const character = static_cast<char>(bytes[i]);
        text += (character >= 32 && character < 127) ? character : '?';
      }
      return "'" + text + "'";
    };

    auto const& root_bytes = backing.parts[part_index(ShadowlandsADTPart::Root)].bytes;

    LogDebug << "[ModernADT][Save] Backing store for '" << root_path << "': ROOT="
             << part_size(ShadowlandsADTPart::Root) << " (starts " << first_bytes(root_bytes) << ")"
             << " TEX0=" << part_size(ShadowlandsADTPart::Tex0)
             << " OBJ0=" << part_size(ShadowlandsADTPart::Obj0)
             << " OBJ1=" << part_size(ShadowlandsADTPart::Obj1)
             << " LOD=" << part_size(ShadowlandsADTPart::Lod) << " byte(s)." << std::endl;

    // A ROOT that is not empty but is not an ADT either would otherwise surface
    // three layers down as "Original ROOT lacks MVER/MHDR/MCIN", which does not
    // say what the file turned out to be. Compared exactly the way parse_chunks()
    // and find_chunk() compare magics, so this cannot disagree with them about a
    // valid part. Reported rather than thrown: a diagnostic must not add a new
    // way for a save to fail, and the writer below already refuses a ROOT it
    // cannot use.
    std::uint32_t first_magic = 0;
    if (root_bytes.size() >= sizeof(first_magic))
      std::memcpy(&first_magic, root_bytes.data(), sizeof(first_magic));

    if (root_bytes.size() >= minimum_adt_part_size
        && first_magic != fourcc('M','V','E','R'))
    {
      LogError << "[ModernADT][Save] The ROOT part read for '" << root_path << "' is "
               << root_bytes.size() << " byte(s) and does not begin with a version chunk"
               << " (first four bytes " << first_bytes(root_bytes) << "); the writer will"
                  " refuse this tile." << std::endl;
    }

    return backing;
  }

  void ShadowlandsADTCodec::saveFromLegacy(
    ShadowlandsADTBackingStore const& backing,
    std::vector<std::uint8_t> const& legacy_adt,
    ModernAdtFdidHints const& fdid_hints,
    ModernAdtSaveReport* report)
  {
    validate_and_save_parts(backing,
                            assemble_parts(backing, legacy_adt, std::nullopt, fdid_hints, report));
  }

  void ShadowlandsADTCodec::saveNativeRoot(
    ShadowlandsADTBackingStore const& backing,
    std::vector<std::uint8_t> root,
    std::vector<std::uint8_t> const& legacy_adt,
    ModernAdtFdidHints const& fdid_hints,
    ModernAdtSaveReport* report)
  {
    validate_and_save_parts(backing,
                            assemble_parts(backing, legacy_adt, std::move(root), fdid_hints, report));
  }
}
