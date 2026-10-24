// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/formats/adt/ModernAdtWriter.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace Noggit::Formats::ADT
{
  namespace
  {
    constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept
    {
      return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8)
           | static_cast<std::uint32_t>(static_cast<unsigned char>(d));
    }

    //! Bytes every chunk spends on its magic and size fields. Distinct from
    //! AdtFormatProfile::chunk_header_size, which is the 128 byte MapChunkHeader
    //! a ROOT MCNK carries inside its payload.
    constexpr std::size_t chunk_tag_size = 8;

    constexpr std::uint32_t MCC_MVER = fourcc('M', 'V', 'E', 'R');
    constexpr std::uint32_t MCC_MHDR = fourcc('M', 'H', 'D', 'R');
    constexpr std::uint32_t MCC_MCIN = fourcc('M', 'C', 'I', 'N');
    constexpr std::uint32_t MCC_MCNK = fourcc('M', 'C', 'N', 'K');
    constexpr std::uint32_t MCC_MH2O = fourcc('M', 'H', '2', 'O');
    constexpr std::uint32_t MCC_MFBO = fourcc('M', 'F', 'B', 'O');
    constexpr std::uint32_t MCC_MCVT = fourcc('M', 'C', 'V', 'T');
    constexpr std::uint32_t MCC_MCNR = fourcc('M', 'C', 'N', 'R');
    constexpr std::uint32_t MCC_MCCV = fourcc('M', 'C', 'C', 'V');
    constexpr std::uint32_t MCC_MCSH = fourcc('M', 'C', 'S', 'H');
    constexpr std::uint32_t MCC_MCSE = fourcc('M', 'C', 'S', 'E');
    constexpr std::uint32_t MCC_MCLY = fourcc('M', 'C', 'L', 'Y');
    constexpr std::uint32_t MCC_MCAL = fourcc('M', 'C', 'A', 'L');
    constexpr std::uint32_t MCC_MDID = fourcc('M', 'D', 'I', 'D');
    constexpr std::uint32_t MCC_MTXP = fourcc('M', 'T', 'X', 'P');
    constexpr std::uint32_t MCC_MDDF = fourcc('M', 'D', 'D', 'F');
    constexpr std::uint32_t MCC_MODF = fourcc('M', 'O', 'D', 'F');
    constexpr std::uint32_t MCC_MCRD = fourcc('M', 'C', 'R', 'D');
    constexpr std::uint32_t MCC_MCRW = fourcc('M', 'C', 'R', 'W');

    constexpr std::size_t mhdr_payload_size = 0x40;
    constexpr std::size_t mcin_payload_size = 256 * sizeof(ENTRY_MCIN);
    constexpr std::size_t mcnk_header_size = sizeof(MapChunkHeader);
    constexpr std::size_t mcse_entry_size = 0x1C;
    constexpr std::uint32_t owned_order[5] = {MCC_MCVT, MCC_MCNR, MCC_MCCV, MCC_MCSH, MCC_MCSE};
    constexpr std::size_t legacy_mcnr_padding_size = 13;

    //! The MCNK flag bits that claim per-chunk liquid (river, ocean, magma,
    //! slime). Meaningful only together with an MH2O chunk: a modern client
    //! answers "does this chunk have liquid" from MH2O, not from MCLQ.
    constexpr std::uint32_t legacy_liquid_flags = 0x4u | 0x8u | 0x10u | 0x20u;
    constexpr std::uint32_t MCC_MCLQ = fourcc('M', 'C', 'L', 'Q');

    // MHDR offsets in Noggit's legacy convention are relative to
    // (MHDR chunk start + 0x14). Real files are self-calibrated instead (see
    // mhdr_offset_basis), so this is only the fallback.
    constexpr std::uint32_t legacy_mhdr_basis_adjust = 0x14;

    struct ChunkView
    {
      std::size_t offset = 0;  // position of the magic
      std::size_t payload = 0; // position of the payload
      std::uint32_t magic = 0;
      std::uint32_t size = 0;

      std::size_t end() const { return payload + size; }
    };

    std::uint32_t read_u32(std::vector<std::uint8_t> const& bytes, std::size_t pos)
    {
      std::uint32_t value = 0;
      std::memcpy(&value, bytes.data() + pos, 4);
      return value;
    }

    void write_u32(std::vector<std::uint8_t>& bytes, std::size_t pos, std::uint32_t value)
    {
      std::memcpy(bytes.data() + pos, &value, 4);
    }

    //! Strict chunk walk over [begin, end).
    //! \param skip_mcnr_padding skip the 13 raw bytes Noggit's legacy serializer
    //!        emits after MCNR. They are padding, not a subchunk.
    std::vector<ChunkView> parse_chunks_strict(std::vector<std::uint8_t> const& bytes,
                                               std::size_t begin,
                                               std::size_t end,
                                               char const* label,
                                               bool skip_mcnr_padding)
    {
      std::vector<ChunkView> result;
      // A legacy MCNK walk starts right after the 128 byte MapChunkHeader, so
      // the header is readable back from `begin` and can vouch for a size-0
      // MCLQ: Noggit's serializer wrote that size field as zero for years while
      // writing the full payload after it, and a walk trusting it reads liquid
      // payload as chunk headers. sizeLiquid is 8 + payload, ofsLiquid points at
      // the chunk.
      std::size_t const mcnk_header =
        skip_mcnr_padding && begin >= sizeof(MapChunkHeader)
          ? begin - sizeof(MapChunkHeader)
          : static_cast<std::size_t>(-1);

      std::size_t pos = begin;
      while (pos + 8 <= end)
      {
        ChunkView chunk;
        chunk.offset = pos;
        chunk.payload = pos + 8;
        std::memcpy(&chunk.magic, bytes.data() + pos, 4);
        std::memcpy(&chunk.size, bytes.data() + pos + 4, 4);
        if (chunk.magic == MCC_MCLQ && chunk.size == 0
            && mcnk_header != static_cast<std::size_t>(-1))
        {
          std::uint32_t ofs_liquid = 0, size_liquid = 0;
          std::memcpy(&ofs_liquid, bytes.data() + mcnk_header + offsetof(MapChunkHeader, ofsLiquid), 4);
          std::memcpy(&size_liquid, bytes.data() + mcnk_header + offsetof(MapChunkHeader, sizeLiquid), 4);
          if (ofs_liquid == pos - mcnk_header + 8 && size_liquid > 8
              && size_liquid - 8 <= end - chunk.payload)
            chunk.size = size_liquid - 8;
        }
        if (chunk.size > end - chunk.payload)
          throw ModernAdtWriterError(std::string(label) + " contains a truncated chunk.");
        result.push_back(chunk);
        pos = chunk.end();
        if (skip_mcnr_padding && chunk.magic == MCC_MCNR
            && legacy_mcnr_padding_size <= end - pos)
          pos += legacy_mcnr_padding_size;
      }
      if (pos != end)
        throw ModernAdtWriterError(std::string(label) + " does not end on a chunk boundary.");
      return result;
    }

    std::vector<ChunkView> parse_chunks(std::vector<std::uint8_t> const& bytes,
                                        std::size_t begin,
                                        std::size_t end,
                                        char const* label,
                                        bool legacy_mcnr_padding = false)
    {
      if (!legacy_mcnr_padding)
        return parse_chunks_strict(bytes, begin, end, label, false);

      // Two MCNK shapes are in the wild: retail puts the next subchunk's header
      // straight after MCNR, Noggit's canonical serializer puts 13 padding bytes
      // there first. They cannot be told apart by inspecting those bytes. The
      // padding is only recognisable while it is zero filled, and nothing
      // guarantees a serializer leaves zeros in a gap it merely reserved, so a
      // content test guesses - and guessing wrong desynchronizes the walk, which
      // then reads payload bytes as a tag and reports a truncated chunk with a
      // nonsense size.
      //
      // Decide by which shape consumes the payload exactly instead. At most one
      // can: skipping 13 bytes in a retail MCNK lands mid-chunk, and not
      // skipping them in a Noggit MCNK walks the padding as a zero sized chunk
      // and then reads five stray bytes plus the next tag as a size. When MCNR is
      // the last subchunk neither skip applies and both agree.
      try
      {
        return parse_chunks_strict(bytes, begin, end, label, false);
      }
      catch (ModernAdtWriterError const& without_padding)
      {
        try
        {
          return parse_chunks_strict(bytes, begin, end, label, true);
        }
        catch (ModernAdtWriterError const& with_padding)
        {
          throw ModernAdtWriterError(std::string(label) + " is malformed under both MCNR"
                                     " padding conventions. Without padding: "
                                     + without_padding.what() + " With padding: "
                                     + with_padding.what());
        }
      }
    }

    std::vector<ChunkView> parse_chunks(std::vector<std::uint8_t> const& bytes, char const* label)
    {
      return parse_chunks(bytes, 0, bytes.size(), label);
    }

    ChunkView const* find_chunk(std::vector<ChunkView> const& chunks, std::uint32_t magic)
    {
      auto const it = std::find_if(chunks.begin(), chunks.end(),
        [magic](ChunkView const& chunk) { return chunk.magic == magic; });
      return it == chunks.end() ? nullptr : &*it;
    }

    std::vector<std::uint8_t> make_chunk(std::uint32_t magic,
                                         std::vector<std::uint8_t> const& payload)
    {
      std::vector<std::uint8_t> result(8 + payload.size());
      auto const size = static_cast<std::uint32_t>(payload.size());
      std::memcpy(result.data(), &magic, 4);
      std::memcpy(result.data() + 4, &size, 4);
      if (!payload.empty())
        std::memcpy(result.data() + 8, payload.data(), payload.size());
      return result;
    }

    std::vector<std::uint8_t> make_chunk_u32(std::uint32_t magic, std::uint32_t value)
    {
      std::vector<std::uint8_t> payload(4);
      std::memcpy(payload.data(), &value, 4);
      return make_chunk(magic, payload);
    }

    void append_range(std::vector<std::uint8_t>& output,
                      std::vector<std::uint8_t> const& input,
                      std::size_t begin,
                      std::size_t end)
    {
      output.insert(output.end(), input.begin() + begin, input.begin() + end);
    }

    void append_chunk(std::vector<std::uint8_t>& output,
                      std::vector<std::uint8_t> const& input,
                      ChunkView const& chunk)
    {
      append_range(output, input, chunk.offset, chunk.end());
    }

    std::vector<std::uint8_t> payload_bytes(std::vector<std::uint8_t> const& bytes,
                                            ChunkView const& chunk)
    {
      return {bytes.begin() + chunk.payload, bytes.begin() + chunk.end()};
    }

    // MHDR offsets are relative to a per-file basis. Derive it from the
    // original file itself (mcin field vs. actual MCIN position) so whatever
    // convention the retail file uses is preserved exactly. Falls back to
    // Noggit's legacy convention.
    std::uint32_t mhdr_offset_basis(std::vector<std::uint8_t> const& bytes,
                                    ChunkView const* mhdr,
                                    ChunkView const* mcin)
    {
      if (mhdr && mhdr->size >= 8)
      {
        std::uint32_t const stored_mcin = read_u32(bytes, mhdr->payload + 4);
        if (mcin && stored_mcin > 0 && mcin->offset >= stored_mcin)
        {
          auto const candidate = static_cast<std::uint32_t>(mcin->offset - stored_mcin);
          if (candidate >= mhdr->offset && candidate <= mcin->offset)
            return candidate;
        }
        return static_cast<std::uint32_t>(mhdr->offset) + legacy_mhdr_basis_adjust;
      }
      return 0;
    }

    std::optional<std::vector<std::uint8_t>> const* owned_payload(
        ModernChunkPayload const& chunk, std::uint32_t magic)
    {
      switch (magic)
      {
      case MCC_MCVT: return &chunk.mcvt;
      case MCC_MCNR: return &chunk.mcnr;
      case MCC_MCCV: return &chunk.mccv;
      case MCC_MCSH: return &chunk.mcsh;
      case MCC_MCSE: return &chunk.mcse;
      default: return nullptr;
      }
    }

    std::optional<std::vector<std::uint8_t>>* owned_payload(
        ModernChunkPayload& chunk, std::uint32_t magic)
    {
      switch (magic)
      {
      case MCC_MCVT: return &chunk.mcvt;
      case MCC_MCNR: return &chunk.mcnr;
      case MCC_MCCV: return &chunk.mccv;
      case MCC_MCSH: return &chunk.mcsh;
      case MCC_MCSE: return &chunk.mcse;
      default: return nullptr;
      }
    }

    // Merges the editor-owned header fields into the original modern header,
    // including the high-resolution hole mask dance (the mask lives in the
    // ofsHeight/ofsNormal fields when flags.high_res_holes is set).
    MapChunkHeader merge_chunk_header(MapChunkHeader original,
                                      MapChunkHeader const& edited,
                                      AdtFormatProfile const& profile)
    {
      MapChunkHeader merged = original;
      std::uint64_t high_res_holes = 0;
      if (profile.supports_high_res_holes && original.flags.flags.high_res_holes)
      {
        high_res_holes = static_cast<std::uint64_t>(original.ofsHeight)
                       | (static_cast<std::uint64_t>(original.ofsNormal) << 32);
      }

      // The flag bits split by owner. Noggit edits impassability, the liquid
      // kinds and MCCV presence (bits 1-6); everything else describes data
      // this writer preserves from the retail original and must not change:
      // has_mcsh (bit 0 - reconciled against the emitted MCSH below), the
      // reserved bits 8-14, unknown_0x80 (bit 7) and high_res_holes (bit 16,
      // 0x10000: the layout of the hole mask in ofsHeight/ofsNormal).
      // do_not_fix_alpha_map (bit 15, 0x8000) is forced set: this writer
      // always emits full-size 64x64 alpha and 512-byte shadow data, and a
      // cleared bit tells the client to expect the 63x63 "fixed" layout,
      // which it avenges with ERROR #134
      // "CMapChunk::UnpackAlphaShadowBits(): Bad genformat". Retail sets the
      // bit on most chunks (at 8-bit depth it also enables the shadow x0.7
      // alpha modification the client expects).
      constexpr std::uint32_t editor_owned_flag_bits = 0x7E;
      merged.flags.value = (edited.flags.value & editor_owned_flag_bits)
                         | (original.flags.value & ~editor_owned_flag_bits);
      merged.flags.flags.do_not_fix_alpha_map = 1;
      merged.ix = edited.ix;
      merged.iy = edited.iy;
      merged.areaid = edited.areaid;
      merged.ypos = edited.ypos;
      merged.nDoodadRefs = edited.nDoodadRefs;
      merged.nMapObjRefs = edited.nMapObjRefs;
      std::copy(std::begin(edited.doodadMapping), std::end(edited.doodadMapping),
                std::begin(merged.doodadMapping));
      std::copy(std::begin(edited.doodadStencil), std::end(edited.doodadStencil),
                std::begin(merged.doodadStencil));

      if (profile.supports_high_res_holes && merged.flags.flags.high_res_holes)
      {
        // The edited legacy header only carries the low-resolution 4x4 hole
        // grid; translate changes into the 64-bit mask, mirroring the
        // verified conversion-path behaviour.
        auto const edited_low_res = edited.holes & 0xFFFF;
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x)
          {
            auto const first = 2 * y * 8 + 2 * x;
            auto const block = (std::uint64_t{3} << first)
                             | (std::uint64_t{3} << (first + 8));
            auto const bit = 1u << (y * 4 + x);
            bool const was_hole = (high_res_holes & block) != 0;
            bool const is_hole = (edited_low_res & bit) != 0;
            if (was_hole != is_hole)
              high_res_holes = is_hole ? high_res_holes | block : high_res_holes & ~block;
          }
        merged.holes = original.holes;
        merged.ofsHeight = static_cast<std::uint32_t>(high_res_holes);
        merged.ofsNormal = static_cast<std::uint32_t>(high_res_holes >> 32);
      }
      else
      {
        merged.holes = (original.holes & 0xFFFF0000u) | (edited.holes & 0xFFFFu);
      }
      return merged;
    }

    struct SubchunkPlacement
    {
      bool present = false;
      std::uint32_t offset_in_payload = 0; // relative to MCNK payload start
      std::uint32_t size = 0;
    };

    // Assembles one ROOT MCNK natively: merged header + owned subchunks from
    // the input + every other subchunk preserved from the original, in
    // original order. Header subchunk offsets are recomputed for the layout
    // actually emitted.
    std::vector<std::uint8_t> assemble_root_mcnk(AdtFormatProfile const& profile,
                                                 std::vector<std::uint8_t> const& original,
                                                 ChunkView const& original_chunk,
                                                 ModernChunkPayload const& input)
    {
      if (original_chunk.size < profile.chunk_header_size)
        throw ModernAdtWriterError("ROOT MCNK is smaller than its header.");
      if (!input.header || input.header->size() != profile.chunk_header_size)
        throw ModernAdtWriterError("Editor payload is missing a ROOT MCNK header.");

      MapChunkHeader original_header{};
      MapChunkHeader edited_header{};
      std::memcpy(&original_header, original.data() + original_chunk.payload, sizeof(original_header));
      std::memcpy(&edited_header, input.header->data(), sizeof(edited_header));

      auto const sub_begin = original_chunk.payload + profile.chunk_header_size;
      auto const sub_end = original_chunk.payload + original_chunk.size;
      auto const original_subs = parse_chunks(original, sub_begin, sub_end, "modern ROOT MCNK");

      std::vector<std::uint8_t> payload;
      // The merged header is serialized last (offsets are only known once the
      // subchunks are laid out); reserve its space now.
      payload.resize(profile.chunk_header_size);

      std::array<SubchunkPlacement, 5> placements{}; // MCVT, MCNR, MCCV, MCSH, MCSE
      auto placement_index = [](std::uint32_t magic) -> std::size_t
      {
        for (std::size_t i = 0; i < 5; ++i)
          if (owned_order[i] == magic)
            return i;
        return static_cast<std::size_t>(-1);
      };

      auto record_placement = [&](std::uint32_t magic, std::uint32_t size)
      {
        auto const index = placement_index(magic);
        if (index == static_cast<std::size_t>(-1))
          return; // not an owned subchunk; no header offset tracks it
        auto& placement = placements[index];
        placement.present = true;
        placement.offset_in_payload = static_cast<std::uint32_t>(payload.size());
        placement.size = size;
      };

      for (auto const& sub : original_subs)
      {
        auto const* owned = owned_payload(input, sub.magic);
        if (owned && owned->has_value())
        {
          auto chunk = make_chunk(sub.magic, **owned);
          record_placement(sub.magic, static_cast<std::uint32_t>((*owned)->size()));
          payload.insert(payload.end(), chunk.begin(), chunk.end());
        }
        else
        {
          record_placement(sub.magic, sub.size);
          append_chunk(payload, original, sub);
        }
      }

      // Owned subchunks the original file did not carry are appended in the
      // canonical order.
      for (std::uint32_t magic : owned_order)
      {
        auto const* owned = owned_payload(input, magic);
        if (!owned || !owned->has_value() || placements[placement_index(magic)].present)
          continue;
        auto chunk = make_chunk(magic, **owned);
        record_placement(magic, static_cast<std::uint32_t>((*owned)->size()));
        payload.insert(payload.end(), chunk.begin(), chunk.end());
      }

      MapChunkHeader merged = merge_chunk_header(original_header, edited_header, profile);

      // Recompute the header offsets for the layout actually emitted. Offsets
      // are relative to the MCNK chunk start (magic), i.e. payload offset + 8.
      // When high-resolution holes are active, ofsHeight/ofsNormal hold the
      // hole mask instead and must not be touched.
      if (!merged.flags.flags.high_res_holes)
      {
        merged.ofsHeight = placements[0].present ? placements[0].offset_in_payload + 8 : original_header.ofsHeight;
        merged.ofsNormal = placements[1].present ? placements[1].offset_in_payload + 8 : original_header.ofsNormal;
      }
      merged.ofsMCCV = placements[2].present ? placements[2].offset_in_payload + 8 : 0;
      merged.ofsShadow = placements[3].present ? placements[3].offset_in_payload + 8 : 0;
      merged.sizeShadow = placements[3].present ? placements[3].size : 0;
      if (placements[4].present)
      {
        merged.ofsSndEmitters = placements[4].offset_in_payload + 8;
        merged.nSndEmitters = placements[4].size / mcse_entry_size;
      }
      else
      {
        merged.ofsSndEmitters = 0;
        merged.nSndEmitters = 0;
      }

      // The flags come from the edited header, but which subchunks exist is
      // decided by the layout above. A bit claiming a subchunk that was not
      // emitted sends the client to read a shadow map or vertex colours that are
      // not there; reconcile the bits with what was actually written. (Liquid
      // bits are reconciled against the tile's MH2O by the caller, which is the
      // only place that knows whether an MH2O ended up in the file.)
      merged.flags.flags.has_mcsh = placements[3].present ? 1u : 0u;
      merged.flags.flags.has_mccv = placements[2].present ? 1u : 0u;

      std::memcpy(payload.data(), &merged, sizeof(merged));

      return make_chunk(MCC_MCNK, payload);
    }

    struct RootLayout
    {
      std::vector<ChunkView> chunks;
      ChunkView const* mver = nullptr;
      ChunkView const* mhdr = nullptr;
      ChunkView const* mcin = nullptr;
      std::vector<ChunkView const*> mcnks;
      ChunkView const* mh2o = nullptr;
      ChunkView const* mfbo = nullptr;
    };

    RootLayout collect_root_layout(std::vector<std::uint8_t> const& bytes, char const* label)
    {
      RootLayout layout;
      layout.chunks = parse_chunks(bytes, label);
      for (auto const& chunk : layout.chunks)
      {
        switch (chunk.magic)
        {
        case MCC_MVER: layout.mver = &chunk; break;
        case MCC_MHDR: layout.mhdr = &chunk; break;
        case MCC_MCIN: layout.mcin = &chunk; break;
        case MCC_MCNK: layout.mcnks.push_back(&chunk); break;
        case MCC_MH2O: layout.mh2o = &chunk; break;
        case MCC_MFBO: layout.mfbo = &chunk; break;
        default: break;
        }
      }
      return layout;
    }

    // Shared MCIN/MHDR repair used by refreshRootBookkeeping and writeRoot's
    // backpatch. positions[i] = {chunk offset, payload size} of MCNK i.
    //
    // MCIN.size describes the whole MCNK chunk including its 8 byte tag, which
    // is what Noggit's legacy serializer has always written
    // (MapChunk::save stores lMCNK_Size + sizeof(sChunkHeader)) and what the
    // conversion library both reads and writes: parseLegacyAdt bounds a chunk
    // walk with entry.offset + entry.size, where offset is the chunk start.
    // Storing the payload size alone left every natively written ROOT 8 bytes
    // short per chunk and made the validator reject correctly built ones.
    void patch_root_bookkeeping(std::vector<std::uint8_t>& bytes,
                                RootLayout const& layout,
                                std::vector<std::pair<std::size_t, std::size_t>> const& mcnk_positions,
                                std::uint32_t basis,
                                std::optional<std::size_t> mh2o_pos,
                                std::optional<std::size_t> mfbo_pos)
    {
      if (!layout.mhdr || layout.mhdr->size < mhdr_payload_size)
        throw ModernAdtWriterError("ROOT MHDR chunk is missing or truncated.");
      if (!layout.mcin || layout.mcin->size != mcin_payload_size)
        throw ModernAdtWriterError("ROOT MCIN chunk is missing or malformed.");
      if (mcnk_positions.size() != 256)
        throw ModernAdtWriterError("ROOT does not contain exactly 256 MCNK chunks.");

      for (std::size_t i = 0; i < 256; ++i)
      {
        auto const entry = layout.mcin->payload + i * sizeof(ENTRY_MCIN);
        write_u32(bytes, entry + offsetof(ENTRY_MCIN, offset),
                  static_cast<std::uint32_t>(mcnk_positions[i].first));
        write_u32(bytes, entry + offsetof(ENTRY_MCIN, size),
                  static_cast<std::uint32_t>(mcnk_positions[i].second + chunk_tag_size));
      }

      write_u32(bytes, layout.mhdr->payload + offsetof(MHDR, mcin),
                static_cast<std::uint32_t>(layout.mcin->offset) - basis);
      write_u32(bytes, layout.mhdr->payload + offsetof(MHDR, mh2o),
                mh2o_pos ? static_cast<std::uint32_t>(*mh2o_pos - basis) : 0u);
      write_u32(bytes, layout.mhdr->payload + offsetof(MHDR, mfbo),
                mfbo_pos ? static_cast<std::uint32_t>(*mfbo_pos - basis) : 0u);
    }

    std::string describe_magic(std::uint32_t magic)
    {
      char buffer[5] = {
        static_cast<char>((magic >> 24) & 0xFF),
        static_cast<char>((magic >> 16) & 0xFF),
        static_cast<char>((magic >> 8) & 0xFF),
        static_cast<char>(magic & 0xFF),
        0};
      return buffer;
    }

    std::vector<std::string> validate_root(AdtFormatProfile const& profile,
                                           std::vector<std::uint8_t> const& bytes)
    {
      std::vector<std::string> problems;
      RootLayout layout;
      try
      {
        layout = collect_root_layout(bytes, "ROOT part");
      }
      catch (std::exception const& e)
      {
        problems.emplace_back(e.what());
        return problems;
      }

      if (!layout.mver)
        problems.emplace_back("MVER chunk is missing.");
      else if (layout.mver->size != 4)
        problems.emplace_back("MVER chunk is not 4 bytes.");
      else if (read_u32(bytes, layout.mver->payload) != profile.mver)
        problems.emplace_back("MVER is " + std::to_string(read_u32(bytes, layout.mver->payload))
                              + ", expected " + std::to_string(profile.mver) + ".");
      if (!layout.mhdr)
        problems.emplace_back("MHDR chunk is missing.");
      else if (layout.mhdr->size != mhdr_payload_size)
        problems.emplace_back("MHDR chunk is not 0x40 bytes.");
      if (!layout.mcin)
        problems.emplace_back("MCIN chunk is missing.");
      else if (layout.mcin->size != mcin_payload_size)
        problems.emplace_back("MCIN chunk is not 256 entries.");
      if (layout.mcnks.size() != profile.mcnk_count)
        problems.emplace_back("ROOT contains " + std::to_string(layout.mcnks.size())
                              + " MCNK chunks, expected " + std::to_string(profile.mcnk_count) + ".");

      // MCIN must describe the actual layout.
      if (layout.mcin && layout.mcin->size == mcin_payload_size
          && layout.mcnks.size() == profile.mcnk_count)
      {
        int reported = 0;
        for (std::size_t i = 0; i < layout.mcnks.size() && reported < 4; ++i)
        {
          auto const entry = layout.mcin->payload + i * sizeof(ENTRY_MCIN);
          std::uint32_t const offset = read_u32(bytes, entry + offsetof(ENTRY_MCIN, offset));
          std::uint32_t const size = read_u32(bytes, entry + offsetof(ENTRY_MCIN, size));
          std::size_t const expected_size = layout.mcnks[i]->size + chunk_tag_size;
          if (offset != layout.mcnks[i]->offset || size != expected_size)
          {
            problems.emplace_back("MCIN entry " + std::to_string(i) + " (offset=" + std::to_string(offset)
                                  + ", size=" + std::to_string(size) + ") does not match the actual MCNK (offset="
                                  + std::to_string(layout.mcnks[i]->offset) + ", size="
                                  + std::to_string(expected_size) + ").");
            ++reported;
          }
        }
      }

      // MHDR offsets must point at the chunks they name (basis self-calibrated
      // exactly like the writer does).
      if (layout.mhdr && layout.mhdr->size >= mhdr_payload_size && layout.mcin)
      {
        std::uint32_t const basis = mhdr_offset_basis(bytes, layout.mhdr, layout.mcin);
        struct FieldCheck { std::uint32_t field_offset; std::uint32_t magic; char const* name; bool present; };
        for (auto const& check : {
               FieldCheck{offsetof(MHDR, mcin), MCC_MCIN, "mcin", true},
               FieldCheck{offsetof(MHDR, mh2o), MCC_MH2O, "mh2o", layout.mh2o != nullptr},
               FieldCheck{offsetof(MHDR, mfbo), MCC_MFBO, "mfbo", layout.mfbo != nullptr} })
        {
          std::uint32_t const value = read_u32(bytes, layout.mhdr->payload + check.field_offset);
          if (value == 0)
          {
            if (check.present)
              problems.emplace_back(std::string("MHDR ") + check.name
                                    + " is 0 but the chunk exists.");
            continue;
          }
          if (!check.present)
          {
            problems.emplace_back(std::string("MHDR ") + check.name
                                  + " points at a chunk that does not exist.");
            continue;
          }
          std::size_t const target = static_cast<std::size_t>(basis) + value;
          auto const matches = std::find_if(layout.chunks.begin(), layout.chunks.end(),
            [&](ChunkView const& chunk) { return chunk.offset == target && chunk.magic == check.magic; });
          if (matches == layout.chunks.end())
            problems.emplace_back(std::string("MHDR ") + check.name
                                  + " does not point at the " + describe_magic(check.magic) + " chunk.");
        }
      }

      // Per-MCNK structural checks (bounded reporting).
      int reported = 0;
      for (std::size_t i = 0; i < layout.mcnks.size() && reported < 8; ++i)
      {
        auto const& chunk = *layout.mcnks[i];
        auto chunk_problem = [&](std::string message)
        {
          problems.emplace_back("MCNK " + std::to_string(i) + ": " + message);
          ++reported;
        };
        if (chunk.size < profile.chunk_header_size)
        {
          chunk_problem("smaller than its header.");
          continue;
        }
        MapChunkHeader header{};
        std::memcpy(&header, bytes.data() + chunk.payload, sizeof(header));

        std::vector<ChunkView> subs;
        try
        {
          subs = parse_chunks(bytes, chunk.payload + profile.chunk_header_size, chunk.end(), "ROOT MCNK");
        }
        catch (std::exception const& e)
        {
          chunk_problem(e.what());
          continue;
        }

        auto sub_at_offset = [&](std::uint32_t offset, std::uint32_t expected_magic) -> ChunkView const*
        {
          if (offset < 8)
            return nullptr;
          std::size_t const target = chunk.payload + (offset - 8);
          for (auto const& sub : subs)
            if (sub.offset == target)
              return sub.magic == expected_magic ? &sub : nullptr;
          return nullptr;
        };

        if (auto const* mcvt = find_chunk(subs, MCC_MCVT))
        {
          if (mcvt->size != profile.heightmap_values * 4)
            chunk_problem("MCVT is " + std::to_string(mcvt->size) + " bytes, expected "
                          + std::to_string(profile.heightmap_values * 4) + ".");
        }
        if (!header.flags.flags.high_res_holes)
        {
          if (header.ofsHeight && !sub_at_offset(header.ofsHeight, MCC_MCVT))
            chunk_problem("ofsHeight does not point at MCVT.");
          if (header.ofsNormal && !sub_at_offset(header.ofsNormal, MCC_MCNR))
            chunk_problem("ofsNormal does not point at MCNR.");
        }
        if (header.ofsMCCV && !sub_at_offset(header.ofsMCCV, MCC_MCCV))
          chunk_problem("ofsMCCV does not point at MCCV.");
        if (auto const* mcsh = find_chunk(subs, MCC_MCSH))
        {
          if (!header.ofsShadow || !sub_at_offset(header.ofsShadow, MCC_MCSH))
            chunk_problem("ofsShadow does not point at MCSH.");
          else if (header.sizeShadow != mcsh->size)
            chunk_problem("sizeShadow does not match the MCSH chunk.");
        }
        if (auto const* mcse = find_chunk(subs, MCC_MCSE))
        {
          if (mcse->size % mcse_entry_size != 0)
            chunk_problem("MCSE size is not a multiple of the entry size.");
          else if (header.nSndEmitters != mcse->size / mcse_entry_size)
            chunk_problem("nSndEmitters does not match the MCSE chunk.");
          if (!header.ofsSndEmitters || !sub_at_offset(header.ofsSndEmitters, MCC_MCSE))
            chunk_problem("ofsSndEmitters does not point at MCSE.");
        }
      }
      return problems;
    }

    std::vector<std::string> validate_texture(AdtFormatProfile const& profile,
                                              std::vector<std::uint8_t> const& bytes)
    {
      std::vector<std::string> problems;
      std::vector<ChunkView> chunks;
      try
      {
        chunks = parse_chunks(bytes, "TEX0 part");
      }
      catch (std::exception const& e)
      {
        problems.emplace_back(e.what());
        return problems;
      }

      if (auto const* mdid = find_chunk(chunks, MCC_MDID); mdid && mdid->size % 4 != 0)
        problems.emplace_back("MDID size is not a multiple of 4.");
      if (auto const* mtxp = find_chunk(chunks, MCC_MTXP); mtxp && mtxp->size % 16 != 0)
        problems.emplace_back("MTXP size is not a multiple of 16.");

      std::size_t mcnk_count = 0;
      int reported = 0;
      for (auto const& chunk : chunks)
      {
        if (chunk.magic != MCC_MCNK)
          continue;
        ++mcnk_count;
        if (reported >= 8)
          continue;
        std::vector<ChunkView> subs;
        try
        {
          subs = parse_chunks(bytes, chunk.payload, chunk.end(), "TEX0 MCNK");
        }
        catch (std::exception const& e)
        {
          problems.emplace_back("MCNK " + std::to_string(mcnk_count - 1) + ": " + e.what());
          ++reported;
          continue;
        }
        if (auto const* mcly = find_chunk(subs, MCC_MCLY); mcly && mcly->size % 16 != 0)
        {
          problems.emplace_back("MCNK " + std::to_string(mcnk_count - 1)
                                + ": MCLY size is not a multiple of 16.");
          ++reported;
        }
      }
      if (mcnk_count != profile.mcnk_count)
        problems.emplace_back("TEX0 contains " + std::to_string(mcnk_count)
                              + " MCNK chunks, expected " + std::to_string(profile.mcnk_count) + ".");
      return problems;
    }

    std::vector<std::string> validate_object(AdtFormatProfile const& profile,
                                             std::vector<std::uint8_t> const& bytes)
    {
      std::vector<std::string> problems;
      std::vector<ChunkView> chunks;
      try
      {
        chunks = parse_chunks(bytes, "OBJ0 part");
      }
      catch (std::exception const& e)
      {
        problems.emplace_back(e.what());
        return problems;
      }

      auto const* mddf = find_chunk(chunks, MCC_MDDF);
      auto const* modf = find_chunk(chunks, MCC_MODF);
      if (mddf && mddf->size % sizeof(ENTRY_MDDF) != 0)
        problems.emplace_back("MDDF size is not a multiple of the entry size.");
      if (modf && modf->size % sizeof(ENTRY_MODF) != 0)
        problems.emplace_back("MODF size is not a multiple of the entry size.");

      std::size_t const doodad_count = mddf ? mddf->size / sizeof(ENTRY_MDDF) : 0;
      std::size_t const wmo_count = modf ? modf->size / sizeof(ENTRY_MODF) : 0;

      std::size_t mcnk_count = 0;
      int reported = 0;
      for (auto const& chunk : chunks)
      {
        if (chunk.magic != MCC_MCNK)
          continue;
        ++mcnk_count;
        if (reported >= 8)
          continue;
        std::vector<ChunkView> subs;
        try
        {
          subs = parse_chunks(bytes, chunk.payload, chunk.end(), "OBJ0 MCNK");
        }
        catch (std::exception const& e)
        {
          problems.emplace_back("MCNK " + std::to_string(mcnk_count - 1) + ": " + e.what());
          ++reported;
          continue;
        }
        auto check_refs = [&](std::uint32_t magic, std::size_t count, char const* name)
        {
          auto const* refs = find_chunk(subs, magic);
          if (!refs)
            return;
          if (refs->size % 4 != 0)
          {
            problems.emplace_back("MCNK " + std::to_string(mcnk_count - 1) + ": " + name
                                  + " size is not a multiple of 4.");
            ++reported;
            return;
          }
          for (std::size_t i = 0; i < refs->size / 4; ++i)
          {
            if (read_u32(bytes, refs->payload + i * 4) >= count)
            {
              problems.emplace_back("MCNK " + std::to_string(mcnk_count - 1) + ": " + name
                                    + " references placement "
                                    + std::to_string(read_u32(bytes, refs->payload + i * 4))
                                    + " of " + std::to_string(count) + ".");
              ++reported;
              return;
            }
          }
        };
        check_refs(MCC_MCRD, doodad_count, "MCRD");
        check_refs(MCC_MCRW, wmo_count, "MCRW");
      }
      if (mcnk_count != profile.mcnk_count)
        problems.emplace_back("OBJ0 contains " + std::to_string(mcnk_count)
                              + " MCNK chunks, expected " + std::to_string(profile.mcnk_count) + ".");
      return problems;
    }
  }

  AdtFormatProfile const& adt_profile_v18()
  {
    static AdtFormatProfile const profile{};
    return profile;
  }

  NativeRootInput ModernAdtWriter::rootInputFromLegacy(
      AdtFormatProfile const& profile,
      std::vector<std::uint8_t> const& legacy_adt)
  {
    auto const chunks = parse_chunks(legacy_adt, "legacy ADT");

    NativeRootInput input;
    auto const* mhdr = find_chunk(chunks, MCC_MHDR);
    if (!mhdr || mhdr->size < mhdr_payload_size)
      throw ModernAdtWriterError("Legacy ADT has no usable MHDR chunk.");
    input.mhdr_flags = read_u32(legacy_adt, mhdr->payload);

    if (auto const* mh2o = find_chunk(chunks, MCC_MH2O))
    {
      input.has_mh2o = true;
      input.mh2o_payload = payload_bytes(legacy_adt, *mh2o);
    }
    if (auto const* mfbo = find_chunk(chunks, MCC_MFBO))
    {
      input.has_mfbo = true;
      input.mfbo_payload = payload_bytes(legacy_adt, *mfbo);
    }

    std::vector<ChunkView const*> mcnks;
    for (auto const& chunk : chunks)
      if (chunk.magic == MCC_MCNK)
        mcnks.push_back(&chunk);
    if (mcnks.size() != profile.mcnk_count)
      throw ModernAdtWriterError("Legacy ADT does not contain exactly "
                                 + std::to_string(profile.mcnk_count) + " MCNK chunks.");

    for (std::size_t i = 0; i < mcnks.size(); ++i)
    {
      auto const& chunk = *mcnks[i];
      if (chunk.size < profile.chunk_header_size)
        throw ModernAdtWriterError("Legacy MCNK " + std::to_string(i) + " is smaller than its header.");

      ModernChunkPayload& payload = input.chunks[i];
      payload.header = std::vector<std::uint8_t>(
          legacy_adt.begin() + chunk.payload,
          legacy_adt.begin() + chunk.payload + profile.chunk_header_size);

      auto const subs = parse_chunks(legacy_adt,
                                     chunk.payload + profile.chunk_header_size,
                                     chunk.end(),
                                     "legacy MCNK",
                                     true);
      for (auto const& sub : subs)
      {
        if (auto* target = owned_payload(payload, sub.magic))
          *target = payload_bytes(legacy_adt, sub);
      }
      if (!payload.mcvt)
        throw ModernAdtWriterError("Legacy MCNK " + std::to_string(i) + " has no MCVT chunk.");
    }
    return input;
  }

    //! A ROOT without MCIN leaves mhdr_offset_basis nothing to calibrate
    //! against, and the legacy fallback is a guess. If the original carries
    //! MH2O or MFBO, the header's stored offsets reveal the basis the file was
    //! actually written with; reuse it so the rewritten header keeps pointing
    //! the client at the same chunks.
    std::uint32_t calibrate_basis_without_mcin(std::vector<std::uint8_t> const& bytes,
                                               RootLayout const& layout,
                                               std::uint32_t fallback)
    {
      if (!layout.mhdr || layout.mhdr->size < mhdr_payload_size)
        return fallback;

      struct basis_probe { std::uint32_t magic; std::size_t field; };
      for (auto const& probe
           : {basis_probe{MCC_MH2O, static_cast<std::size_t>(offsetof(MHDR, mh2o))},
              basis_probe{MCC_MFBO, static_cast<std::size_t>(offsetof(MHDR, mfbo))}})
      {
        for (auto const& chunk : layout.chunks)
        {
          if (chunk.magic != probe.magic)
            continue;
          std::uint32_t const stored = read_u32(bytes, layout.mhdr->payload + probe.field);
          if (stored == 0 || chunk.offset < stored)
            continue;
          auto const candidate = static_cast<std::uint32_t>(chunk.offset - stored);
          if (candidate >= layout.mhdr->offset && candidate <= chunk.offset)
            return candidate;
        }
      }
      return fallback;
    }

  std::vector<std::uint8_t> ModernAdtWriter::writeRoot(
      AdtFormatProfile const& profile,
      std::vector<std::uint8_t> const& original_root,
      NativeRootInput const& input)
  {
    auto const original_layout = collect_root_layout(original_root, "original ROOT");
    // MCIN is optional: retail ROOT files may omit it (probe: azeroth tiles
    // whose ROOT walks MVER MHDR MH2O MFBO MCNK... with no MCIN at all), and
    // refusing those forced whole tiles down the lossy synthesized path.
    if (!original_layout.mver || !original_layout.mhdr)
      throw ModernAdtWriterError("Original ROOT lacks MVER/MHDR; cannot rewrite natively.");
    if (original_layout.mhdr->size < mhdr_payload_size)
      throw ModernAdtWriterError("Original ROOT MHDR is truncated.");
    if (original_layout.mcnks.size() != profile.mcnk_count)
      throw ModernAdtWriterError("Original ROOT does not contain exactly "
                                 + std::to_string(profile.mcnk_count) + " MCNK chunks.");

    std::uint32_t basis = mhdr_offset_basis(original_root, original_layout.mhdr,
                                            original_layout.mcin);
    if (!original_layout.mcin)
      basis = calibrate_basis_without_mcin(original_root, original_layout, basis);

    // MCIN entry template: keep flags/asyncID of the original entries.
    std::vector<std::uint8_t> mcin_template(mcin_payload_size, 0);
    if (original_layout.mcin && original_layout.mcin->size == mcin_payload_size)
      mcin_template = payload_bytes(original_root, *original_layout.mcin);

    std::vector<std::uint8_t> output;
    std::vector<std::pair<std::size_t, std::size_t>> mcnk_positions; // {chunk offset, payload size}
    std::optional<std::size_t> mh2o_pos;
    std::optional<std::size_t> mfbo_pos;
    std::size_t mcnk_index = 0;

    for (auto const& chunk : original_layout.chunks)
    {
      switch (chunk.magic)
      {
      case MCC_MVER:
      {
        auto bytes = make_chunk_u32(MCC_MVER, profile.mver);
        output.insert(output.end(), bytes.begin(), bytes.end());
        break;
      }
      case MCC_MHDR:
      {
        std::vector<std::uint8_t> payload(mhdr_payload_size, 0);
        std::memcpy(payload.data(), original_root.data() + chunk.payload,
                    std::min<std::size_t>(chunk.size, mhdr_payload_size));
        write_u32(payload, offsetof(MHDR, flags), input.mhdr_flags);
        auto bytes = make_chunk(MCC_MHDR, payload);
        output.insert(output.end(), bytes.begin(), bytes.end());
        if (!original_layout.mcin)
        {
          // The output contract - validatePart and Noggit's split-ADT gate -
          // wants MCIN even when the retail original had none; the client
          // accepts either form. Emit a zeroed MCIN right after MHDR (retail's
          // own position for it); patch_root_bookkeeping fills in real indices.
          auto mcin_bytes = make_chunk(MCC_MCIN, mcin_template);
          output.insert(output.end(), mcin_bytes.begin(), mcin_bytes.end());
        }
        break;
      }
      case MCC_MCIN:
      {
        auto bytes = make_chunk(MCC_MCIN, mcin_template);
        output.insert(output.end(), bytes.begin(), bytes.end());
        break;
      }
      case MCC_MCNK:
      {
        auto bytes = assemble_root_mcnk(profile, original_root, chunk, input.chunks[mcnk_index++]);
        mcnk_positions.emplace_back(output.size(), bytes.size() - 8);
        output.insert(output.end(), bytes.begin(), bytes.end());
        break;
      }
      case MCC_MH2O:
      {
        mh2o_pos = output.size();
        auto bytes = input.has_mh2o
          ? make_chunk(MCC_MH2O, input.mh2o_payload)
          : std::vector<std::uint8_t>{};
        if (bytes.empty())
          append_chunk(bytes, original_root, chunk);
        output.insert(output.end(), bytes.begin(), bytes.end());
        break;
      }
      case MCC_MFBO:
      {
        mfbo_pos = output.size();
        auto bytes = input.has_mfbo
          ? make_chunk(MCC_MFBO, input.mfbo_payload)
          : std::vector<std::uint8_t>{};
        if (bytes.empty())
          append_chunk(bytes, original_root, chunk);
        output.insert(output.end(), bytes.begin(), bytes.end());
        break;
      }
      default:
        // MTXP, MHID and any future/unknown chunk: preserved byte-for-byte.
        append_chunk(output, original_root, chunk);
        break;
      }
    }

    if (mcnk_index != profile.mcnk_count)
      throw ModernAdtWriterError("Original ROOT MCNK count changed while writing.");

    // When the result carries no MH2O - the original had none and the editor
    // produced none - any liquid bit left in a chunk header points the client at
    // a liquid table that is not there, which it dereferences. The flags are the
    // header's first member, eight bytes into the chunk.
    if (!input.has_mh2o && !mh2o_pos)
    {
      for (auto const& position : mcnk_positions)
      {
        std::uint32_t flags = 0;
        std::memcpy(&flags, output.data() + position.first + 8, sizeof(flags));
        flags &= ~legacy_liquid_flags;
        std::memcpy(output.data() + position.first + 8, &flags, sizeof(flags));
      }
    }

    // Original files may legitimately lack MH2O/MFBO; append them when the
    // editor produced data for chunks the original did not carry.
    if (input.has_mh2o && !mh2o_pos)
    {
      mh2o_pos = output.size();
      auto bytes = make_chunk(MCC_MH2O, input.mh2o_payload);
      output.insert(output.end(), bytes.begin(), bytes.end());
    }
    if (input.has_mfbo && !mfbo_pos)
    {
      mfbo_pos = output.size();
      auto bytes = make_chunk(MCC_MFBO, input.mfbo_payload);
      output.insert(output.end(), bytes.begin(), bytes.end());
    }

    // Backpatch MCIN and MHDR against the emitted layout.
    auto const written_layout = [&]
    {
      RootLayout layout;
      layout.chunks = parse_chunks(output, "written ROOT");
      for (auto const& written : layout.chunks)
      {
        switch (written.magic)
        {
        case MCC_MHDR: layout.mhdr = &written; break;
        case MCC_MCIN: layout.mcin = &written; break;
        default: break;
        }
      }
      return layout;
    }();
    patch_root_bookkeeping(output, written_layout, mcnk_positions, basis, mh2o_pos, mfbo_pos);

    // Nothing leaves this function unvalidated. A failure here is a writer
    // defect (ModernAdtWriterError), not suspect editor data, so callers may
    // still fall back to the conversion path.
    try
    {
      validatePartOrThrow(profile, output, AdtPartExpectation::Root, "written ROOT");
    }
    catch (ModernAdtValidationError const& error)
    {
      throw ModernAdtWriterError(
          std::string("Native ROOT writer produced an invalid part: ") + error.what());
    }
    return output;
  }

  void ModernAdtWriter::refreshRootBookkeeping(
      AdtFormatProfile const& profile,
      std::vector<std::uint8_t>& root)
  {
    auto layout = collect_root_layout(root, "ROOT part");
    if (!layout.mhdr)
      throw ModernAdtWriterError("ROOT lacks MHDR; cannot refresh bookkeeping.");

    // Retail ROOT files may omit MCIN (the same variant writeRoot handles by
    // synthesizing it). The merged fallback rewrites the original ROOT in
    // place, so it has to grow the chunk the same way: the output contract -
    // validatePart and the split-ADT gate - wants MCIN, and refusing here is
    // what left 969 tile saves with nothing on disk after the native path had
    // already failed ("ROOT lacks MHDR/MCIN; cannot refresh bookkeeping").
    std::optional<std::uint32_t> calibrated_basis;
    if (!layout.mcin)
    {
      if (layout.mhdr->size < mhdr_payload_size)
        throw ModernAdtWriterError("ROOT MHDR is truncated; cannot refresh bookkeeping.");
      // Calibrate against the original stored offsets BEFORE the insertion
      // shifts every chunk after MHDR; the basis is a fixed file anchor and
      // stays valid for the shifted positions.
      calibrated_basis = calibrate_basis_without_mcin(
          root, layout, mhdr_offset_basis(root, layout.mhdr, nullptr));
      auto const insert_at = layout.mhdr->offset + chunk_tag_size + layout.mhdr->size;
      if (insert_at > root.size())
        throw ModernAdtWriterError("ROOT MHDR chunk extends past the part.");
      std::vector<std::uint8_t> mcin_payload(mcin_payload_size, 0);
      auto const mcin_bytes = make_chunk(MCC_MCIN, mcin_payload);
      root.insert(root.begin() + static_cast<std::ptrdiff_t>(insert_at),
                  mcin_bytes.begin(), mcin_bytes.end());
      layout = collect_root_layout(root, "ROOT part");
      if (!layout.mcin)
        throw ModernAdtWriterError("ROOT MCIN synthesis failed.");
    }

    std::uint32_t const basis = calibrated_basis
      ? *calibrated_basis
      : mhdr_offset_basis(root, layout.mhdr, layout.mcin);

    std::vector<std::pair<std::size_t, std::size_t>> positions;
    positions.reserve(layout.mcnks.size());
    for (auto const* chunk : layout.mcnks)
      positions.emplace_back(chunk->offset, chunk->size);

    std::optional<std::size_t> const mh2o = layout.mh2o
      ? std::optional<std::size_t>(layout.mh2o->offset) : std::nullopt;
    std::optional<std::size_t> const mfbo = layout.mfbo
      ? std::optional<std::size_t>(layout.mfbo->offset) : std::nullopt;

    patch_root_bookkeeping(root, layout, positions, basis, mh2o, mfbo);
  }

  std::vector<std::string> ModernAdtWriter::validatePart(
      AdtFormatProfile const& profile,
      std::vector<std::uint8_t> const& bytes,
      AdtPartExpectation expectation)
  {
    switch (expectation)
    {
    case AdtPartExpectation::Root:    return validate_root(profile, bytes);
    case AdtPartExpectation::Texture: return validate_texture(profile, bytes);
    case AdtPartExpectation::Object:  return validate_object(profile, bytes);
    case AdtPartExpectation::Generic:
    default:
      try
      {
        parse_chunks(bytes, "ADT part");
        return {};
      }
      catch (std::exception const& e)
      {
        return {e.what()};
      }
    }
  }

  void ModernAdtWriter::validatePartOrThrow(
      AdtFormatProfile const& profile,
      std::vector<std::uint8_t> const& bytes,
      AdtPartExpectation expectation,
      char const* part_label)
  {
    auto const problems = validatePart(profile, bytes, expectation);
    if (problems.empty())
      return;
    std::ostringstream message;
    message << part_label << " failed pre-write validation:";
    for (auto const& problem : problems)
      message << "\n  - " << problem;
    throw ModernAdtValidationError(message.str());
  }

  namespace
  {
    constexpr std::uint32_t MCC_MLDD = fourcc('M', 'L', 'D', 'D');
    constexpr std::uint32_t MCC_MLDX = fourcc('M', 'L', 'D', 'X');
    constexpr std::uint32_t MCC_MLDL = fourcc('M', 'L', 'D', 'L');
    constexpr std::uint32_t MCC_MDLI = fourcc('M', 'D', 'L', 'I');
    constexpr std::uint32_t MCC_MLDB = fourcc('M', 'L', 'D', 'B');
    constexpr std::uint32_t MCC_MLMD = fourcc('M', 'L', 'M', 'D');
    constexpr std::uint32_t MCC_MLMX = fourcc('M', 'L', 'M', 'X');
    constexpr std::uint32_t MCC_MLMB = fourcc('M', 'L', 'M', 'B');
    constexpr std::uint32_t MCC_MWDR = fourcc('M', 'W', 'D', 'R');
    constexpr std::uint32_t MCC_MWDS = fourcc('M', 'W', 'D', 'S');
    constexpr std::uint32_t MCC_MLFD = fourcc('M', 'L', 'F', 'D');

    //! MLMD/MLMX record: a MODF placement without its bounding box, which is
    //! what MLMX carries next to it instead.
    struct LodMapObject
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
    static_assert(sizeof(LodMapObject) == 40, "MLMD entry layout");

    //! MLDX/MLMX record: transformed bounding box plus its radius.
    struct LodExtent
    {
      float min[3];
      float max[3];
      float radius;
    };
    static_assert(sizeof(LodExtent) == 28, "MLDX/MLMX entry layout");

    //! MWDR record: inclusive [begin, end] range into the MWDS pool. An empty
    //! range is begin > end; {1, 0} reads as zero elements under signed,
    //! unsigned and widening arithmetic alike.
    struct LodWmoRange
    {
      std::uint32_t begin;
      std::uint32_t end;
    };
    static_assert(sizeof(LodWmoRange) == 8, "MWDR entry layout");

    template<typename Entry>
    std::vector<Entry> chunk_records(std::vector<std::uint8_t> const& bytes,
                                     ChunkView const& chunk,
                                     char const* label)
    {
      if (chunk.size % sizeof(Entry))
        throw ModernAdtWriterError(std::string(label) + " has an invalid record size.");
      std::vector<Entry> records(chunk.size / sizeof(Entry));
      if (chunk.size)
        std::memcpy(records.data(), bytes.data() + chunk.payload, chunk.size);
      return records;
    }

    template<typename Entry>
    std::vector<Entry> payload_records(std::vector<std::uint8_t> const& payload,
                                       char const* label)
    {
      if (payload.size() % sizeof(Entry))
        throw ModernAdtWriterError(std::string(label) + " payload has an invalid record size.");
      std::vector<Entry> records(payload.size() / sizeof(Entry));
      if (!payload.empty())
        std::memcpy(records.data(), payload.data(), payload.size());
      return records;
    }

    template<typename Entry>
    std::vector<std::uint8_t> records_bytes(std::vector<Entry> const& records)
    {
      std::vector<std::uint8_t> bytes(records.size() * sizeof(Entry));
      if (!bytes.empty())
        std::memcpy(bytes.data(), records.data(), bytes.size());
      return bytes;
    }

    std::uint64_t lod_placement_key(std::uint32_t name_id, std::uint32_t unique_id)
    {
      return (static_cast<std::uint64_t>(unique_id) << 32) | name_id;
    }

    std::vector<std::uint8_t> lod_replace_chunk(
      std::vector<std::uint8_t> const& input,
      std::uint32_t magic,
      std::vector<std::uint8_t> const& replacement)
    {
      auto const chunks = parse_chunks(input, "split ADT part");
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

    std::vector<std::uint8_t> lod_remove_chunk(
      std::vector<std::uint8_t> const& input,
      std::uint32_t magic)
    {
      auto const chunks = parse_chunks(input, "split ADT part");
      std::vector<std::uint8_t> output;
      for (auto const& chunk : chunks)
      {
        if (chunk.magic == magic)
          continue;
        append_range(output, input, chunk.offset, chunk.payload + chunk.size);
      }
      return output;
    }

    std::string describe_chunk_list(std::vector<ChunkView> const& chunks)
    {
      std::ostringstream out;
      for (std::size_t i = 0; i < chunks.size(); ++i)
      {
        if (i)
          out << ' ';
        out << describe_magic(chunks[i].magic) << '(' << chunks[i].size << ')';
      }
      return out.str();
    }

    std::array<float, 3> lod_server_position(float const (&position)[3])
    {
      constexpr float map_midpoint = 17066.666f;
      return {map_midpoint - position[2], map_midpoint - position[0], position[1]};
    }

    LodExtent lod_moved_extent(LodExtent const& old_extent,
                               float const (&old_position)[3],
                               float const (&new_position)[3],
                               float scale_ratio,
                               bool rotation_changed)
    {
      auto const before = lod_server_position(old_position);
      auto const after = lod_server_position(new_position);
      LodExtent result = old_extent;
      float radius = 0.0f;
      for (int axis = 0; axis < 3; ++axis)
      {
        radius = std::max(radius, std::abs(old_extent.min[axis] - before[axis]));
        radius = std::max(radius, std::abs(old_extent.max[axis] - before[axis]));
      }
      for (int axis = 0; axis < 3; ++axis)
      {
        if (rotation_changed)
        {
          result.min[axis] = after[axis] - radius * scale_ratio;
          result.max[axis] = after[axis] + radius * scale_ratio;
        }
        else
        {
          result.min[axis] = after[axis] + (old_extent.min[axis] - before[axis]) * scale_ratio;
          result.max[axis] = after[axis] + (old_extent.max[axis] - before[axis]) * scale_ratio;
        }
      }
      result.radius = old_extent.radius * scale_ratio;
      return result;
    }

    LodExtent lod_map_object_extent(ENTRY_MODF const& placement)
    {
      LodExtent result{};
      constexpr float map_midpoint = 17066.666f;
      auto const& low = placement.extents[0];
      auto const& high = placement.extents[1];
      result.min[0] = map_midpoint - high.z;
      result.max[0] = map_midpoint - low.z;
      result.min[1] = map_midpoint - high.x;
      result.max[1] = map_midpoint - low.x;
      result.min[2] = low.y;
      result.max[2] = high.y;
      result.radius = std::sqrt(
        std::pow(result.max[0] - result.min[0], 2.0f)
        + std::pow(result.max[1] - result.min[1], 2.0f)
        + std::pow(result.max[2] - result.min[2], 2.0f));
      return result;
    }

    //! One side's three LOD bands inside MLFD. Retail bands are contiguous:
    //! offset[b] is the cumulative length before b and the three lengths sum
    //! to the list size. Anything else is a layout this writer does not
    //! claim to understand.
    struct LodBandTable
    {
      std::uint32_t offset[3];
      std::uint32_t length[3];
    };

    bool lod_bands_valid(LodBandTable const& bands, std::size_t total)
    {
      std::size_t cumulative = 0;
      for (int b = 0; b < 3; ++b)
      {
        if (static_cast<std::size_t>(bands.offset[b]) != cumulative)
          return false;
        cumulative += bands.length[b];
        if (cumulative > total)
          return false;
      }
      return cumulative == total;
    }

    std::size_t lod_band_of(LodBandTable const& bands, std::size_t index)
    {
      if (index < static_cast<std::size_t>(bands.length[0]))
        return 0;
      if (index < static_cast<std::size_t>(bands.length[0]) + bands.length[1])
        return 1;
      return 2;
    }
  }

  Obj1LodRewrite rewriteObj1Lod(
      std::vector<std::uint8_t> const& obj1,
      std::vector<std::uint8_t> const& mddf_payload,
      std::vector<std::uint8_t> const& modf_payload)
  {
    Obj1LodRewrite result;
    result.bytes = obj1;

    auto const chunks = parse_chunks(obj1, "modern OBJ1");
    auto const* mldd = find_chunk(chunks, MCC_MLDD);
    auto const* mldx = find_chunk(chunks, MCC_MLDX);
    auto const* mldl = find_chunk(chunks, MCC_MLDL);
    auto const* mdli = find_chunk(chunks, MCC_MDLI);
    auto const* mldb = find_chunk(chunks, MCC_MLDB);
    auto const* mlmd = find_chunk(chunks, MCC_MLMD);
    auto const* mlmx = find_chunk(chunks, MCC_MLMX);
    auto const* mlmb = find_chunk(chunks, MCC_MLMB);
    auto const* mwdr = find_chunk(chunks, MCC_MWDR);
    auto const* mwds = find_chunk(chunks, MCC_MWDS);
    auto const* mlfd = find_chunk(chunks, MCC_MLFD);

    bool const doodad_side = mldd && mldx;
    bool const wmo_side = mlmd && mlmx;
    if (!doodad_side && !wmo_side)
    {
      if (mldl || mdli || mldb || mlmb || mwdr || mwds || mlfd)
      {
        result.reason = "OBJ1 carries LOD companion chunks without a complete MLDD/MLDX"
                        " or MLMD/MLMX pair, so its LOD lists were passed through as-is."
                        " Layout: " + describe_chunk_list(chunks);
      }
      return result;
    }

    std::vector<std::string> problems;
    std::vector<ENTRY_MDDF> old_doodads;
    std::vector<LodExtent> old_doodad_extents;
    std::vector<std::uint32_t> old_doodad_lod;
    std::vector<std::uint32_t> old_doodad_indices;
    std::vector<std::uint8_t> old_doodad_batches;
    std::vector<LodMapObject> old_map_objects;
    std::vector<LodExtent> old_map_extents;
    std::vector<std::uint8_t> old_map_classes;
    std::vector<LodWmoRange> old_map_ranges;

    if (doodad_side)
    {
      if (mldd->size % sizeof(ENTRY_MDDF))
        problems.emplace_back("MLDD size is not a multiple of the 36 byte MDDF record");
      else
        old_doodads = chunk_records<ENTRY_MDDF>(obj1, *mldd, "OBJ1 MLDD");
      if (mldx->size % sizeof(LodExtent))
        problems.emplace_back("MLDX size is not a multiple of the 28 byte extent record");
      else
        old_doodad_extents = chunk_records<LodExtent>(obj1, *mldx, "OBJ1 MLDX");
      if (problems.empty() && old_doodads.size() != old_doodad_extents.size())
        problems.emplace_back("MLDD (" + std::to_string(old_doodads.size())
                              + " entries) and MLDX (" + std::to_string(old_doodad_extents.size())
                              + " extents) disagree");
      if (mldl)
      {
        if (mldl->size % sizeof(std::uint32_t)
            || (problems.empty() && mldl->size / sizeof(std::uint32_t) != old_doodads.size()))
          problems.emplace_back("MLDL does not hold one float per MLDD entry");
        else
          old_doodad_lod = chunk_records<std::uint32_t>(obj1, *mldl, "OBJ1 MLDL");
      }
      if (mldb)
      {
        if (problems.empty() && mldb->size != old_doodads.size())
          problems.emplace_back("MLDB is " + std::to_string(mldb->size)
                                + " byte(s) for " + std::to_string(old_doodads.size())
                                + " MLDD entries");
        else
          old_doodad_batches.assign(obj1.begin() + mldb->payload,
                                    obj1.begin() + mldb->payload + mldb->size);
      }
      if (mdli)
      {
        if (mdli->size % sizeof(std::uint32_t))
          problems.emplace_back("MDLI size is not a multiple of 4");
        else
          old_doodad_indices = chunk_records<std::uint32_t>(obj1, *mdli, "OBJ1 MDLI");
      }
    }
    if (wmo_side)
    {
      if (mlmd->size % sizeof(LodMapObject))
        problems.emplace_back("MLMD size is not a multiple of the 40 byte record");
      else
        old_map_objects = chunk_records<LodMapObject>(obj1, *mlmd, "OBJ1 MLMD");
      if (mlmx->size % sizeof(LodExtent))
        problems.emplace_back("MLMX size is not a multiple of the 28 byte extent record");
      else
        old_map_extents = chunk_records<LodExtent>(obj1, *mlmx, "OBJ1 MLMX");
      if (problems.empty() && old_map_objects.size() != old_map_extents.size())
        problems.emplace_back("MLMD (" + std::to_string(old_map_objects.size())
                              + " entries) and MLMX (" + std::to_string(old_map_extents.size())
                              + " extents) disagree");
      if (mlmb)
      {
        if (problems.empty() && mlmb->size != old_map_objects.size())
          problems.emplace_back("MLMB is " + std::to_string(mlmb->size)
                                + " byte(s) for " + std::to_string(old_map_objects.size())
                                + " MLMD entries");
        else
          old_map_classes.assign(obj1.begin() + mlmb->payload,
                                 obj1.begin() + mlmb->payload + mlmb->size);
      }
      if (mwdr)
      {
        if (mwdr->size % sizeof(LodWmoRange)
            || (problems.empty() && mwdr->size / sizeof(LodWmoRange) != old_map_objects.size()))
          problems.emplace_back("MWDR does not hold one range per MLMD entry");
        else
          old_map_ranges = chunk_records<LodWmoRange>(obj1, *mwdr, "OBJ1 MWDR");
      }
    }

    std::array<std::uint32_t, 12> counts{};
    LodBandTable doodad_bands{};
    LodBandTable wmo_bands{};
    if (!mlfd)
      problems.emplace_back("the LOD placement lists exist but MLFD is missing");
    else if (mlfd->size != counts.size() * sizeof(std::uint32_t))
      problems.emplace_back("MLFD is " + std::to_string(mlfd->size) + " byte(s), not 48");
    else
    {
      std::memcpy(counts.data(), obj1.data() + mlfd->payload, sizeof(counts));
      for (int b = 0; b < 3; ++b)
      {
        doodad_bands.offset[b] = counts[b];
        doodad_bands.length[b] = counts[3 + b];
        wmo_bands.offset[b] = counts[6 + b];
        wmo_bands.length[b] = counts[9 + b];
      }
      if (problems.empty() && doodad_side && !lod_bands_valid(doodad_bands, old_doodads.size()))
        problems.emplace_back("MLFD's doodad bands do not tile MLDD");
      if (problems.empty() && wmo_side && !lod_bands_valid(wmo_bands, old_map_objects.size()))
        problems.emplace_back("MLFD's WMO bands do not tile MLMD");
    }

    if (!problems.empty())
    {
      std::ostringstream reason;
      reason << "OBJ1 holds LOD chunks in a layout this writer does not rebuild (";
      for (std::size_t i = 0; i < problems.size(); ++i)
        reason << (i ? "; " : "") << problems[i];
      reason << "), so its LOD lists were passed through as-is. Layout: "
             << describe_chunk_list(chunks);
      result.reason = reason.str();
      return result;
    }

    // The merged OBJ0 lists are the truth about which placements exist now.
    // A retail LOD entry survives when its (nameID, uniqueID) is still
    // placed; its transform comes from the merged entry, everything the LOD
    // side owns (flags, band, companions) is carried over. Editor-added
    // placements stay out of the LOD lists: they render normally up close
    // and only Blizzard's offline LOD baker decides what represents them at
    // distance - inventing extents, classes and band memberships for them is
    // what previously desynchronized MLDB/MLMB/MWDR from their lists.
    auto const new_doodads = payload_records<ENTRY_MDDF>(mddf_payload, "merged MDDF");
    auto const new_map_objects = payload_records<ENTRY_MODF>(modf_payload, "merged MODF");
    std::unordered_map<std::uint64_t, std::size_t> doodad_by_key;
    std::unordered_map<std::uint64_t, std::size_t> map_object_by_key;
    for (std::size_t i = 0; i < new_doodads.size(); ++i)
      doodad_by_key.emplace(lod_placement_key(new_doodads[i].nameID, new_doodads[i].uniqueID), i);
    for (std::size_t i = 0; i < new_map_objects.size(); ++i)
      map_object_by_key.emplace(
        lod_placement_key(new_map_objects[i].nameID, new_map_objects[i].uniqueID), i);

    std::vector<ENTRY_MDDF> out_doodads;
    std::vector<LodExtent> out_doodad_extents;
    std::vector<std::uint32_t> out_doodad_lod;
    std::vector<std::uint32_t> out_doodad_indices;
    std::vector<std::uint8_t> out_doodad_batches;
    std::array<std::size_t, 3> doodad_survivors{};
    if (doodad_side)
    {
      for (std::size_t i = 0; i < old_doodads.size(); ++i)
      {
        auto const& old = old_doodads[i];
        auto const it = doodad_by_key.find(lod_placement_key(old.nameID, old.uniqueID));
        if (it == doodad_by_key.end())
          continue; // the editor deleted this placement
        auto const& merged = new_doodads[it->second];

        ENTRY_MDDF entry = merged;
        entry.flags = old.flags; // the LOD list keeps its own flag word

        bool const rotation_changed = !std::equal(std::begin(old.rot), std::end(old.rot),
                                                  std::begin(merged.rot));
        bool const unchanged = !rotation_changed
          && std::equal(std::begin(old.pos), std::end(old.pos), std::begin(merged.pos))
          && old.scale == merged.scale;
        LodExtent extent = old_doodad_extents[i];
        if (!unchanged)
          extent = lod_moved_extent(extent, old.pos, merged.pos,
                                    static_cast<float>(merged.scale)
                                      / static_cast<float>(std::max(1u, static_cast<unsigned>(old.scale))),
                                    rotation_changed);

        out_doodads.push_back(entry);
        out_doodad_extents.push_back(extent);
        if (mldl)
          out_doodad_lod.push_back(old_doodad_lod[i]);
        if (mldb)
          out_doodad_batches.push_back(old_doodad_batches[i]);
        if (mdli)
          out_doodad_indices.push_back(i < old_doodad_indices.size()
                                         ? old_doodad_indices[i] : 0xFFFFFFFFu);
        ++doodad_survivors[lod_band_of(doodad_bands, i)];
      }
    }

    std::vector<LodMapObject> out_map_objects;
    std::vector<LodExtent> out_map_extents;
    std::vector<std::uint8_t> out_map_classes;
    std::vector<LodWmoRange> out_map_ranges;
    std::array<std::size_t, 3> map_object_survivors{};
    if (wmo_side)
    {
      for (std::size_t i = 0; i < old_map_objects.size(); ++i)
      {
        auto const& old = old_map_objects[i];
        auto const it = map_object_by_key.find(lod_placement_key(old.nameID, old.uniqueID));
        if (it == map_object_by_key.end())
          continue;
        auto const& merged = new_map_objects[it->second];

        LodMapObject entry{};
        std::memcpy(&entry, &merged, 32);
        std::memcpy(reinterpret_cast<std::uint8_t*>(&entry) + 32,
                    reinterpret_cast<std::uint8_t const*>(&merged) + 56, 8);
        // Keep shipped bounds byte-for-byte when the placement has not moved.
        LodExtent extent = std::memcmp(&entry, &old, sizeof(entry)) == 0
          ? old_map_extents[i]
          : lod_map_object_extent(merged);

        out_map_objects.push_back(entry);
        out_map_extents.push_back(extent);
        if (mlmb)
          out_map_classes.push_back(old_map_classes[i]);
        if (mwdr)
          out_map_ranges.push_back(old_map_ranges[i]);
        ++map_object_survivors[lod_band_of(wmo_bands, i)];
      }
    }

    if (doodad_side)
    {
      for (int b = 0; b < 3; ++b)
        counts[3 + b] = static_cast<std::uint32_t>(doodad_survivors[b]);
      counts[0] = 0;
      counts[1] = counts[3];
      counts[2] = counts[3] + counts[4];
    }
    if (wmo_side)
    {
      for (int b = 0; b < 3; ++b)
        counts[9 + b] = static_cast<std::uint32_t>(map_object_survivors[b]);
      counts[6] = 0;
      counts[7] = counts[9];
      counts[8] = counts[9] + counts[10];
    }

    auto output = obj1;
    auto put = [&output](std::uint32_t magic, std::vector<std::uint8_t> const& payload)
    {
      output = lod_replace_chunk(output, magic, make_chunk(magic, payload));
    };
    if (doodad_side)
    {
      put(MCC_MLDD, records_bytes(out_doodads));
      put(MCC_MLDX, records_bytes(out_doodad_extents));
      if (mldl)
        put(MCC_MLDL, records_bytes(out_doodad_lod));
      if (mldb)
        put(MCC_MLDB, out_doodad_batches);
      if (mdli)
        put(MCC_MDLI, records_bytes(out_doodad_indices));
    }
    if (wmo_side)
    {
      put(MCC_MLMD, records_bytes(out_map_objects));
      put(MCC_MLMX, records_bytes(out_map_extents));
      if (mlmb)
        put(MCC_MLMB, out_map_classes);
      if (mwdr)
        put(MCC_MWDR, records_bytes(out_map_ranges));
    }
    put(MCC_MLFD, records_bytes(std::vector<std::uint32_t>(counts.begin(), counts.end())));

    result.bytes = std::move(output);
    result.rewritten = true;
    return result;
  }

  Obj0CompanionRewrite rewriteObj0Companions(
      std::vector<std::uint8_t> const& obj0,
      std::vector<std::uint8_t> const& original_obj0)
  {
    Obj0CompanionRewrite result;
    result.bytes = obj0;

    auto const original_chunks = parse_chunks(original_obj0, "original OBJ0");
    auto const* original_modf_chunk = find_chunk(original_chunks, MCC_MODF);
    auto const* original_mlmb = find_chunk(original_chunks, MCC_MLMB);
    auto const* original_mwdr = find_chunk(original_chunks, MCC_MWDR);
    if (!original_mlmb && !original_mwdr)
      return result; // nothing per-WMO to keep in step

    std::vector<std::string> notes;
    std::vector<ENTRY_MODF> original_modf;
    if (original_modf_chunk)
    {
      if (original_modf_chunk->size % sizeof(ENTRY_MODF))
        notes.emplace_back("the original MODF chunk has an invalid record size, so its"
                           " companion chunks were dropped");
      else
        original_modf = chunk_records<ENTRY_MODF>(original_obj0, *original_modf_chunk,
                                                  "original OBJ0 MODF");
    }

    bool const mlmb_usable = original_mlmb
      && notes.empty() && original_mlmb->size == original_modf.size();
    if (original_mlmb && !mlmb_usable && notes.empty())
      notes.emplace_back("MLMB is " + std::to_string(original_mlmb->size) + " byte(s) for "
                         + std::to_string(original_modf.size())
                         + " MODF placement(s), which contradicts the one-byte-per-WMO"
                           " layout, so MLMB and MWDR were dropped rather than written"
                           " mismatched");
    bool mwdr_usable = original_mwdr && notes.empty()
      && original_mwdr->size % sizeof(LodWmoRange) == 0
      && original_mwdr->size / sizeof(LodWmoRange) == original_modf.size();
    if (original_mwdr && !mwdr_usable && notes.empty())
      notes.emplace_back("MWDR does not hold one range per original MODF placement, so"
                         " MLMB and MWDR were dropped rather than written mismatched");

    // The most common retail MLMB class is the default for editor-added WMOs;
    // with no retail byte to learn from, dropping MLMB is more honest than a
    // hardcoded class.
    std::uint8_t default_class = 0;
    bool have_default_class = false;
    if (mlmb_usable)
    {
      std::array<std::size_t, 256> histogram{};
      for (std::size_t i = 0; i < original_mlmb->size; ++i)
        ++histogram[original_obj0[original_mlmb->payload + i]];
      std::size_t best = 0;
      for (std::size_t value = 0; value < 256; ++value)
        if (histogram[value] > best)
        {
          best = histogram[value];
          default_class = static_cast<std::uint8_t>(value);
          have_default_class = true;
        }
    }
    bool mlmb_rebuilt = mlmb_usable && have_default_class;
    if (mlmb_usable && !have_default_class && !original_modf.empty())
      notes.emplace_back("MLMB had no retail class byte to default editor-added WMOs to,"
                         " so it was dropped");

    auto const new_chunks = parse_chunks(obj0, "merged OBJ0");
    auto const* new_modf_chunk = find_chunk(new_chunks, MCC_MODF);
    std::vector<ENTRY_MODF> new_modf;
    if (new_modf_chunk)
    {
      if (new_modf_chunk->size % sizeof(ENTRY_MODF))
        notes.emplace_back("the merged MODF chunk has an invalid record size, so the"
                           " companion chunks were dropped");
      else
        new_modf = chunk_records<ENTRY_MODF>(obj0, *new_modf_chunk, "merged OBJ0 MODF");
    }
    else if (original_modf_chunk || original_mlmb || original_mwdr)
      notes.emplace_back("the merged OBJ0 has no MODF chunk, so the companion chunks were"
                         " dropped");

    std::unordered_map<std::uint64_t, std::size_t> original_by_key;
    for (std::size_t i = 0; i < original_modf.size(); ++i)
      original_by_key.emplace(
        lod_placement_key(original_modf[i].nameID, original_modf[i].uniqueID), i);

    std::vector<std::uint8_t> rebuilt_mlmb;
    std::vector<LodWmoRange> rebuilt_mwdr;
    if (!notes.empty())
    {
      mlmb_rebuilt = false;
      mwdr_usable = false;
    }
    if (mlmb_rebuilt || mwdr_usable)
    {
      for (auto const& placement : new_modf)
      {
        auto const it = original_by_key.find(
          lod_placement_key(placement.nameID, placement.uniqueID));
        bool const carried = it != original_by_key.end();
        if (mlmb_rebuilt)
          rebuilt_mlmb.push_back(carried
            ? original_obj0[original_mlmb->payload + it->second]
            : default_class);
        if (mwdr_usable)
        {
          LodWmoRange range{1, 0}; // empty inclusive range for editor-added WMOs
          if (carried)
            std::memcpy(&range,
                        original_obj0.data() + original_mwdr->payload
                          + it->second * sizeof(LodWmoRange),
                        sizeof(range));
          rebuilt_mwdr.push_back(range);
        }
        if (!carried)
        {
          ++result.defaults_used;
          if (!result.first_default_fdid)
            result.first_default_fdid = placement.nameID;
        }
      }
    }

    auto output = obj0;
    if (mlmb_rebuilt && !rebuilt_mlmb.empty())
      output = lod_replace_chunk(output, MCC_MLMB, make_chunk(MCC_MLMB, rebuilt_mlmb));
    else if (original_mlmb)
      output = lod_remove_chunk(output, MCC_MLMB);
    if (mwdr_usable && !rebuilt_mwdr.empty())
      output = lod_replace_chunk(output, MCC_MWDR, make_chunk(MCC_MWDR, records_bytes(rebuilt_mwdr)));
    else if (original_mwdr)
      output = lod_remove_chunk(output, MCC_MWDR);
    result.bytes = std::move(output);

    std::ostringstream note;
    for (std::size_t i = 0; i < notes.size(); ++i)
      note << (i ? "; " : "") << notes[i];
    result.note = note.str();
    return result;
  }
}
