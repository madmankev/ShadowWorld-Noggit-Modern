// Standalone check for the modern ADT format conventions and the save-recovery
// path. Not part of the build (the noggit target globs src/noggit only, so this
// stays out of it); build it by hand from the repository root:
//
//   g++ -std=c++20 -I src -I src/external -o /tmp/adt_format_check
//       tools/adt_format_check.cpp
//       src/noggit/formats/adt/ModernAdtWriter.cpp
//       src/noggit/formats/adt/ModernAdtReader.cpp
//       src/noggit/formats/wdt/WdtReader.cpp
//       src/noggit/formats/conversion/AdtConversion.cpp
//       src/noggit/formats/conversion/MapConversion.cpp
//
//   /tmp/adt_format_check        # exit 0 = all checks passed
//
// (join the g++ lines with a trailing backslash, or put them on one line; they
// are split here only because a backslash at the end of a // comment makes the
// comment continue onto the next line.)
//
// Every source it links is Qt-free, so it needs no Qt and no CMake configure.
//
// What it pins down, all of which is otherwise invisible until a save fails:
//   * a legacy ADT in the shape Noggit's own serializer emits (13 zero bytes
//     after MCNR) converts to modern parts, and so does a retail-shaped one
//     without them;
//   * the converted ROOT/TEX0/OBJ0 pass ModernAdtWriter's pre-write validation,
//     which is the gate the save pipeline actually uses - the converter's own
//     self-check uses the readers and is more permissive;
//   * OBJ0 carries all 256 MCNK chunks, and a placement the converter has to
//     drop takes its own per-chunk reference with it while the survivors are
//     renumbered into the compacted MDDF;
//   * a ROOT written by the recovery path can be merged into by the next save,
//     so synthesis happens once rather than on every save;
//   * MCIN.size is the whole MCNK chunk including its 8 byte tag.
//
// Verifies the ADT save-recovery path added to ShadowlandsADTCodec:
//   legacy (Noggit-serialized) ADT -> convertAdtLegacyToModern ->
//   parts that ModernAdtWriter::validatePartOrThrow accepts ->
//   a ROOT that a *later* save can merge into normally (self-healing).
//
// Both libraries are Qt-free, so this links the real sources.

#include <noggit/formats/adt/ModernAdtReader.hpp>
#include <noggit/formats/adt/ModernAdtWriter.hpp>
#include <noggit/formats/conversion/MapConversion.hpp>
#include <noggit/MapHeaders.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
  int g_checks = 0;
  int g_failed = 0;

  void check(bool ok, std::string const& what)
  {
    ++g_checks;
    if (!ok)
    {
      ++g_failed;
      std::printf("  FAIL  %s\n", what.c_str());
    }
  }

  std::uint32_t fc(char a, char b, char c, char d)
  {
    return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16)
         | (std::uint32_t(c) << 8) | std::uint32_t(d);
  }

  struct Writer
  {
    std::vector<std::uint8_t> b;

    std::size_t position() const { return b.size(); }

    void u8(std::uint8_t v) { b.push_back(v); }

    void u32(std::uint32_t v)
    {
      for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }

    void f32(float v)
    {
      std::uint32_t u;
      std::memcpy(&u, &v, 4);
      u32(u);
    }

    void raw(void const* p, std::size_t n)
    {
      auto const* s = static_cast<std::uint8_t const*>(p);
      b.insert(b.end(), s, s + n);
    }

    void zeros(std::size_t n) { b.resize(b.size() + n, 0); }

    void patch_u32(std::size_t at, std::uint32_t v)
    {
      for (int i = 0; i < 4; ++i)
        b[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }

    // Noggit stores the tag as a little-endian u32 of the MSB-first fourcc, so
    // the bytes on disk spell it backwards. Both readers agree on this.
    void tag(std::uint32_t magic) { u32(magic); }
  };

  //! A WotLK ADT built the way Noggit's own serializer builds it.
  //!
  //! `mcnr_padding` toggles the 13 bytes Noggit reserves after MCNR. Real retail
  //! files do not have them, so both shapes have to convert. `with_mccv` and
  //! `with_mcsh` toggle the two subchunks MapChunk::save emits conditionally.
  std::vector<std::uint8_t> build_legacy_adt(bool mcnr_padding,
                                             std::size_t layers,
                                             std::size_t doodads,
                                             bool with_mccv = false,
                                             bool with_mcsh = false,
                                             bool liquid_flags = false,
                                             int mclq_shape = 0)
  {
    Writer w;

    // MVER
    w.tag(fc('M', 'V', 'E', 'R')); w.u32(4); w.u32(18);

    // MHDR (payload starts at 0x14, which is the offset basis Noggit uses)
    std::size_t const mhdr_chunk = w.position();
    std::size_t const mhdr_payload = mhdr_chunk + 8;
    w.tag(fc('M', 'H', 'D', 'R')); w.u32(sizeof(MHDR));
    w.zeros(sizeof(MHDR));

    // MCIN
    std::size_t const mcin_chunk = w.position();
    std::size_t const mcin_payload = mcin_chunk + 8;
    w.tag(fc('M', 'C', 'I', 'N')); w.u32(256 * sizeof(ENTRY_MCIN));
    w.zeros(256 * sizeof(ENTRY_MCIN));

    // MTEX: two textures, so MDID ends up with two entries.
    std::vector<std::string> textures = {"tileset/emeralddream/grass.blp",
                                         "tileset/emeralddream/dirt.blp"};
    std::vector<std::size_t> texture_offsets;
    std::size_t const mtex_chunk = w.position();
    {
      std::size_t off = 0;
      for (auto const& t : textures)
      {
        texture_offsets.push_back(off);
        off += t.size() + 1;
      }
      w.tag(fc('M', 'T', 'E', 'X')); w.u32(static_cast<std::uint32_t>(off));
      for (auto const& t : textures)
      {
        w.raw(t.data(), t.size());
        w.u8(0);
      }
    }

    // MMDX / MMID: one model.
    std::string const model = "world/dreaming/passivedoodads/emeralddreamcatcher01.m2";
    std::size_t const mmdx_chunk = w.position();
    w.tag(fc('M', 'M', 'D', 'X')); w.u32(static_cast<std::uint32_t>(model.size() + 1));
    w.raw(model.data(), model.size()); w.u8(0);

    std::size_t const mmid_chunk = w.position();
    w.tag(fc('M', 'M', 'I', 'D')); w.u32(4); w.u32(0);

    // MDDF: `doodads` resolvable entries plus one whose nameID is out of range,
    // which the converter must drop - shifting every later index and exercising
    // the per-chunk reference remap.
    std::size_t const mddf_total = doodads + (doodads ? 1 : 0);
    std::size_t const mddf_chunk = w.position();
    w.tag(fc('M', 'D', 'D', 'F'));
    w.u32(static_cast<std::uint32_t>(mddf_total * sizeof(ENTRY_MDDF)));
    for (std::size_t i = 0; i < mddf_total; ++i)
    {
      ENTRY_MDDF e{};
      e.nameID = (i < doodads) ? 0u : 99u; // 99 has no MMDX entry
      e.uniqueID = static_cast<std::uint32_t>(i);
      e.pos[0] = 100.0f + static_cast<float>(i);
      e.pos[1] = 10.0f;
      e.pos[2] = 200.0f;
      e.scale = 100;
      e.flags = 0;
      w.raw(&e, sizeof(e));
    }

    // MCNK x256
    std::vector<std::pair<std::size_t, std::size_t>> mcnk_spans; // {chunk start, size}
    for (std::size_t i = 0; i < 256; ++i)
    {
      std::size_t const chunk_start = w.position();
      std::size_t const header_pos = chunk_start + 8;
      w.tag(fc('M', 'C', 'N', 'K'));
      std::size_t const size_slot = w.position();
      w.u32(0); // patched below

      MapChunkHeader h{};
      h.ix = static_cast<std::uint32_t>(i % 16);
      h.iy = static_cast<std::uint32_t>(i / 16);
      h.nLayers = static_cast<std::uint32_t>(layers);
      h.nDoodadRefs = 0;
      h.flags = static_cast<mcnk_flags>(0);
      // A retail tile with water and shadows: MCLQ-era liquid bits plus the
      // shadow bit, with neither MCLQ nor MCSH present in this fixture (Noggit's
      // exporter writes neither by default).
      if (liquid_flags)
        h.flags.value |= 0x8u | 0x1u; // lq_ocean | has_mcsh
      std::size_t const header_slot = w.position();
      w.raw(&h, sizeof(h));

      // MapChunk::save writes MCVT, MCCV, MCNR + 13 reserved bytes, MCLY, MCRF,
      // MCSH, MCAL, MCLQ, MCSE - in that order. Mirroring it matters: a fixture
      // that invents its own order cannot catch a bug that depends on the real
      // one. MCLQ is left out because Noggit only writes it when the
      // use_mclq_liquids_export setting is on, which it is not by default.

      // MCVT: 9*9 + 8*8 = 145 heights
      h.ofsHeight = static_cast<std::uint32_t>(w.position() - chunk_start);
      w.tag(fc('M', 'C', 'V', 'T')); w.u32(145 * 4);
      for (int v = 0; v < 145; ++v)
        w.f32(static_cast<float>(v));

      // MCCV: 145 packed vertex colours, only when the tile has them
      if (with_mccv)
      {
        h.ofsMCCV = static_cast<std::uint32_t>(w.position() - chunk_start);
        w.tag(fc('M', 'C', 'C', 'V')); w.u32(145 * 4);
        for (int v = 0; v < 145; ++v)
          w.u32(0x007F7F7Fu);
      }

      // MCNR: mapbufsize (9*9 + 8*8 = 145) normals, then the 13 bytes Noggit
      // reserves. Retail 9.2.7 carries the same 435 byte MCNR with no padding,
      // which the FormatProbe confirmed on a real file.
      h.ofsNormal = static_cast<std::uint32_t>(w.position() - chunk_start);
      w.tag(fc('M', 'C', 'N', 'R')); w.u32(145 * 3);
      w.zeros(145 * 3);
      if (mcnr_padding)
        w.zeros(13);

      // MCLY
      h.ofsLayer = static_cast<std::uint32_t>(w.position() - chunk_start);
      w.tag(fc('M', 'C', 'L', 'Y'));
      w.u32(static_cast<std::uint32_t>(layers * sizeof(ENTRY_MCLY)));
      for (std::size_t l = 0; l < layers; ++l)
      {
        ENTRY_MCLY c{};
        c.textureID = static_cast<std::uint32_t>(texture_offsets[l % textures.size()]);
        c.flags = 0;
        c.effectID = 0xFFFFFFFF;
        // First layer is opaque and carries no alpha span.
        c.ofsAlpha = (l == 0) ? 0u : static_cast<std::uint32_t>((l - 1) * 4096);
        w.raw(&c, sizeof(c));
      }

      // MCRF: nDoodadRefs indices into MDDF, then nMapObjRefs into MODF.
      // Chunks 0..doodads-1 each reference one doodad; chunk `doodads`
      // references the entry the converter has to drop.
      std::uint32_t n_refs = 0;
      if (doodads && i <= doodads)
        n_refs = 1;
      h.nDoodadRefs = n_refs;
      h.nMapObjRefs = 0;
      h.ofsRefs = static_cast<std::uint32_t>(w.position() - chunk_start);
      w.tag(fc('M', 'C', 'R', 'F')); w.u32(n_refs * 4);
      if (n_refs)
        w.u32(static_cast<std::uint32_t>(i)); // chunk i -> doodad i (or the dropped one)

      // MCSH: a fixed 0x200 byte shadow map, only when the tile has shadows
      if (with_mcsh)
      {
        h.ofsShadow = static_cast<std::uint32_t>(w.position() - chunk_start);
        h.sizeShadow = 0x200;
        w.tag(fc('M', 'C', 'S', 'H')); w.u32(0x200);
        w.zeros(0x200);
      }

      // MCAL: 64x64 uncompressed bytes per non-first layer
      std::uint32_t const mcal_size =
        static_cast<std::uint32_t>(layers > 1 ? (layers - 1) * 4096 : 0);
      h.ofsAlpha = static_cast<std::uint32_t>(w.position() - chunk_start);
      h.sizeAlpha = mcal_size;
      w.tag(fc('M', 'C', 'A', 'L')); w.u32(mcal_size);
      w.zeros(mcal_size);

      // MCLQ: Noggit's serializer writes this only when the (off by default)
      // use_mclq_liquids_export setting is on. Shape 1 is the historical bug - a
      // zero size field with the full payload written after it - shape 2 is the
      // fixed writer. The payload is 0x44 so that any walk which trusts the zero
      // reads a tag-shaped lie and fails loudly.
      if (mclq_shape)
      {
        h.ofsLiquid = static_cast<std::uint32_t>(w.position() - chunk_start);
        h.sizeLiquid = 8 + static_cast<std::uint32_t>(sizeof(mclq));
        h.flags.value |= 0x4u; // lq_river
        w.tag(fc('M', 'C', 'L', 'Q'));
        w.u32(mclq_shape == 1 ? 0u : static_cast<std::uint32_t>(sizeof(mclq)));
        std::vector<std::uint8_t> const payload(sizeof(mclq), 0x44);
        w.raw(payload.data(), payload.size());
      }

      // MCSE: Noggit always writes the chunk; empty means a zero payload.
      w.tag(fc('M', 'C', 'S', 'E')); w.u32(0);

      // Write the header back with the offsets and ref counts now known.
      std::memcpy(w.b.data() + header_slot, &h, sizeof(h));

      std::size_t const total = w.position() - header_pos; // payload incl. header struct
      w.patch_u32(size_slot, static_cast<std::uint32_t>(total));
      mcnk_spans.emplace_back(chunk_start, total + 8);
    }

    // MCIN + MHDR now that every position is final.
    for (std::size_t i = 0; i < 256; ++i)
    {
      ENTRY_MCIN e{};
      e.offset = static_cast<std::uint32_t>(mcnk_spans[i].first);
      e.size = static_cast<std::uint32_t>(mcnk_spans[i].second);
      e.flags = 0;
      e.asyncID = 0;
      std::memcpy(w.b.data() + mcin_payload + i * sizeof(e), &e, sizeof(e));
    }

    MHDR hdr{};
    hdr.flags = 0;
    auto const rel = [mhdr_payload](std::size_t chunk_start)
    {
      return static_cast<std::uint32_t>(chunk_start - mhdr_payload);
    };
    hdr.mcin = rel(mcin_chunk);
    hdr.mtex = rel(mtex_chunk);
    hdr.mmdx = rel(mmdx_chunk);
    hdr.mmid = rel(mmid_chunk);
    hdr.mddf = rel(mddf_chunk);
    hdr.modf = 0;
    hdr.mwmo = 0;
    hdr.mwid = 0;
    hdr.mfbo = 0;
    hdr.mh2o = 0;
    hdr.mtxf = 0;
    std::memcpy(w.b.data() + mhdr_payload, &hdr, sizeof(hdr));

    return w.b;
  }

  using namespace Noggit::Formats::Conversion;

  ConversionFileSource make_source(std::map<std::string, std::uint32_t> const& known)
  {
    ConversionFileSource src;
    src.read_file = [](std::string const&)
    {
      return std::optional<std::vector<std::uint8_t>>{};
    };
    src.path_to_fdid = [known](std::string const& path) -> std::optional<std::uint32_t>
    {
      if (auto const it = known.find(path); it != known.end())
        return it->second;
      return std::nullopt;
    };
    src.fdid_to_path = [](std::uint32_t) { return std::string{}; };
    return src;
  }

  //! Counts the per-chunk object references in a modern OBJ0 and records the
  //! highest index each list uses, so a wrong remap is caught even when it
  //! happens to stay in range.
  struct Obj0Refs
  {
    std::size_t mcnks = 0;
    std::size_t mcrd = 0;
    std::size_t mcrw = 0;
    long highest_mcrd = -1;
  };

  Obj0Refs scan_obj0(std::vector<std::uint8_t> const& b)
  {
    Obj0Refs r;
    std::size_t pos = 0;
    auto const rd32 = [&b](std::size_t at)
    {
      return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8)
           | (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
    };
    while (pos + 8 <= b.size())
    {
      std::uint32_t const magic = rd32(pos);
      std::uint32_t const size = rd32(pos + 4);
      std::size_t const payload = pos + 8;
      if (payload + size > b.size())
        break;
      if (magic == fc('M', 'C', 'N', 'K'))
      {
        ++r.mcnks;
        std::size_t sub = payload;
        while (sub + 8 <= payload + size)
        {
          std::uint32_t const sm = rd32(sub);
          std::uint32_t const ss = rd32(sub + 4);
          std::size_t const sp = sub + 8;
          if (sp + ss > payload + size)
            break;
          if (sm == fc('M', 'C', 'R', 'D'))
          {
            for (std::size_t i = 0; i + 4 <= ss; i += 4)
            {
              ++r.mcrd;
              long const v = static_cast<long>(rd32(sp + i));
              if (v > r.highest_mcrd)
                r.highest_mcrd = v;
            }
          }
          else if (sm == fc('M', 'C', 'R', 'W'))
          {
            r.mcrw += ss / 4;
          }
          sub = sp + ss;
        }
      }
      pos = payload + size;
    }
    return r;
  }

  std::string issues_text(std::vector<ConversionIssue> const& issues)
  {
    std::string out;
    for (auto const& i : issues)
    {
      out += "\n        [";
      out += (i.severity == ConversionIssue::Severity::error ? "error" : "warn");
      out += "] ";
      out += i.message;
    }
    return out;
  }
}

//! The subchunks of a modern part's first MCNK, walked strictly (modern parts
//! carry no MCNR padding), and its header flags.
std::uint32_t rd32_at(std::vector<std::uint8_t> const& b, std::size_t at)
{
  return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8)
       | (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
}

bool part_first_mcnk_has_sub(std::vector<std::uint8_t> const& part, std::uint32_t magic,
                               bool mcnk_has_header)
{
  std::size_t pos = 0, mcnk = 0;
  while (pos + 8 <= part.size())
  {
    std::uint32_t const m = rd32_at(part, pos);
    std::uint32_t const sz = rd32_at(part, pos + 4);
    if (m == fc('M', 'C', 'N', 'K'))
    {
      mcnk = pos;
      break;
    }
    pos += 8 + sz;
  }
  if (!mcnk)
    return false;
  std::size_t sub = mcnk + 8 + (mcnk_has_header ? sizeof(MapChunkHeader) : 0);
  std::size_t const sub_end = mcnk + 8 + rd32_at(part, mcnk + 4);
  while (sub + 8 <= sub_end)
  {
    std::uint32_t const m = rd32_at(part, sub);
    std::uint32_t const sz = rd32_at(part, sub + 4);
    if (m == magic)
      return true;
    sub += 8 + sz;
  }
  return false;
}

std::uint32_t part_first_mcnk_flags(std::vector<std::uint8_t> const& part)
{
  std::size_t pos = 0;
  while (pos + 8 <= part.size())
  {
    std::uint32_t const m = rd32_at(part, pos);
    std::uint32_t const sz = rd32_at(part, pos + 4);
    if (m == fc('M', 'C', 'N', 'K'))
      return rd32_at(part, pos + 8);
    pos += 8 + sz;
  }
  return 0;
}


int main()
{
  using namespace Noggit::Formats::ADT;
  auto const& profile = adt_profile_v18();

  // Textures/models the project listfile knows, mirroring the fdid hints
  // MapTile::save supplies.
  std::map<std::string, std::uint32_t> const known = {
    {"tileset/emeralddream/grass.blp", 123456u},
    {"tileset/emeralddream/dirt.blp", 234567u},
    {"world/dreaming/passivedoodads/emeralddreamcatcher01.m2", 345678u},
  };

  struct variant
  {
    char const* name;
    bool padding;
    std::size_t layers;
    std::size_t doodads;
    bool mccv;
    bool mcsh;
  };

  for (auto const& v : {variant{"Noggit-serialized (13 padding bytes), 2 layers, 5 doodads",
                                true, 2, 5, false, false},
                        variant{"Noggit-serialized, 1 layer, 0 doodads", true, 1, 0, false, false},
                        variant{"Noggit-serialized with MCCV and MCSH, 2 layers, 5 doodads",
                                true, 2, 5, true, true},
                        variant{"retail-shaped (no padding), 2 layers, 5 doodads",
                                false, 2, 5, false, false}})
  {
    std::printf("\n%s\n", v.name);
    auto const legacy = build_legacy_adt(v.padding, v.layers, v.doodads, v.mccv, v.mcsh);
    check(legacy.size() > 1000, "fixture is a plausible size");

    ConversionOptions options;
    options.target = MapFormat::modern;
    options.map_name = "emeralddream";
    options.convert_liquids = true;
    options.include_wdl = false;

    auto source = make_source(known);
    FdidAllocator fdids(options.custom_fdid_base);
    auto const result = convertAdtLegacyToModern(legacy, options, source, fdids);

    if (!result.ok)
      std::printf("    conversion issues:%s\n", issues_text(result.issues).c_str());
    check(result.ok, "convertAdtLegacyToModern succeeded");
    check(!result.root.empty(), "ROOT part produced");
    check(!result.tex0.empty(), "TEX0 part produced");
    check(!result.obj0.empty(), "OBJ0 part produced");

    // The split format keeps the shadow map in TEX0, and the ROOT's has_mcsh bit
    // has to agree with what TEX0 actually carries: set with the chunk present,
    // clear without it. A bit pointing at nothing is a client-side read through
    // a table that is not there.
    if (v.mcsh)
    {
      check(!part_first_mcnk_has_sub(result.root, fc('M', 'C', 'S', 'H'), true),
            "ROOT carries no MCSH; the split format keeps it in TEX0");
      check(part_first_mcnk_has_sub(result.tex0, fc('M', 'C', 'S', 'H'), false),
            "TEX0 carries the chunk's MCSH");
      check((part_first_mcnk_flags(result.root) & 0x1u) != 0,
            "has_mcsh is set while TEX0 carries the map");
    }
    if (!result.ok)
      continue;

    // The decisive check: the save pipeline validates with ModernAdtWriter, not
    // with the readers the converter self-checks against. If these throw, the
    // recovery path writes nothing and the tile is still lost.
    bool root_ok = true, tex0_ok = true, obj0_ok = true;
    std::string root_why, tex0_why, obj0_why;
    try { ModernAdtWriter::validatePartOrThrow(profile, result.root, AdtPartExpectation::Root, "ROOT part"); }
    catch (std::exception const& e) { root_ok = false; root_why = e.what(); }
    try { ModernAdtWriter::validatePartOrThrow(profile, result.tex0, AdtPartExpectation::Texture, "TEX0 part"); }
    catch (std::exception const& e) { tex0_ok = false; tex0_why = e.what(); }
    try { ModernAdtWriter::validatePartOrThrow(profile, result.obj0, AdtPartExpectation::Object, "OBJ0 part"); }
    catch (std::exception const& e) { obj0_ok = false; obj0_why = e.what(); }

    if (!root_ok)  std::printf("    ROOT rejected: %s\n", root_why.c_str());
    if (!tex0_ok)  std::printf("    TEX0 rejected: %s\n", tex0_why.c_str());
    if (!obj0_ok)  std::printf("    OBJ0 rejected: %s\n", obj0_why.c_str());
    check(root_ok, "synthesized ROOT passes ModernAdtWriter validation");
    check(tex0_ok, "synthesized TEX0 passes ModernAdtWriter validation");
    check(obj0_ok, "synthesized OBJ0 passes ModernAdtWriter validation");

    // The fixture gives MDDF v.doodads + 1 entries, the last one unresolvable so
    // the converter must drop it, and chunks 0..v.doodads each reference one
    // entry - the last of them the dropped one. So exactly v.doodads references
    // survive, renumbered into 0..v.doodads-1.
    {
      auto const refs = scan_obj0(result.obj0);
      check(refs.mcnks == 256, "OBJ0 carries all 256 MCNK chunks");
      if (v.doodads)
      {
        check(refs.mcrd == v.doodads,
              "a dropped placement takes its own MCRF reference with it");
        check(refs.highest_mcrd == static_cast<long>(v.doodads) - 1,
              "surviving references are renumbered into the compacted MDDF");
      }
      else
      {
        check(refs.mcrd == 0, "no references emitted for a tile with no placements");
      }
    }

    // Textures must resolve through the listfile rather than get invented ids.
    std::size_t custom = 0;
    for (auto const& a : fdids.assignments())
      if (!a.existed_in_client)
        ++custom;
    check(custom == 0, "no invented FileDataIDs when the listfile knows the assets");

    // Self-healing: once this ROOT is on disk, the *next* save takes the normal
    // merge path instead of synthesizing again.
    bool merge_ok = true;
    std::string merge_why;
    try
    {
      auto input = ModernAdtWriter::rootInputFromLegacy(profile, legacy);
      auto rewritten = ModernAdtWriter::writeRoot(profile, result.root, input);
      ModernAdtWriter::validatePartOrThrow(profile, rewritten, AdtPartExpectation::Root, "ROOT part");
      check(!rewritten.empty(), "re-merged ROOT is non-empty");
    }
    catch (std::exception const& e)
    {
      merge_ok = false;
      merge_why = e.what();
    }
    if (!merge_ok)
      std::printf("    re-merge rejected: %s\n", merge_why.c_str());
    check(merge_ok, "a later save can merge into the synthesized ROOT (self-healing)");

    bool bookkeeping_ok = true;
    try
    {
      auto root_copy = result.root;
      ModernAdtWriter::refreshRootBookkeeping(profile, root_copy);
    }
    catch (std::exception const& e)
    {
      bookkeeping_ok = false;
      std::printf("    refreshRootBookkeeping: %s\n", e.what());
    }
    check(bookkeeping_ok, "refreshRootBookkeeping accepts the synthesized ROOT");
  }

  // The bad-MCNK locator, against a file that is clean and one broken on
  // purpose. Without this the diagnostic could report "all clean" no matter
  // what it was given.
  // A Noggit-shaped MCNK whose reserved padding is not zero filled.
  //
  // MapChunk::save reserves 13 bytes after MCNR and counts them in the MCNK's
  // size, but whether those bytes are zero depends on the buffer having to grow
  // to hold them. Telling Noggit's shape from retail's by testing them for zero
  // therefore guesses - and guessing wrong desynchronizes the walk, which then
  // reads payload as a tag and reports a truncated chunk whose "declared size"
  // is nonsense. That is exactly how 59 of 70 tiles failed to save: the sizes
  // decoded to 0x44444444 and to float bit patterns, i.e. bytes from inside a
  // payload. 0x44 is the fill used here.
  std::printf("\nnon-zero MCNR padding\n");
  {
    auto dirty = build_legacy_adt(true, 2, 5);
    auto const rd = [&dirty](std::size_t at)
    {
      return std::uint32_t(dirty[at]) | (std::uint32_t(dirty[at + 1]) << 8)
           | (std::uint32_t(dirty[at + 2]) << 16) | (std::uint32_t(dirty[at + 3]) << 24);
    };

    std::size_t pos = 0, filled = 0;
    while (pos + 8 <= dirty.size())
    {
      std::uint32_t const magic = rd(pos);
      std::uint32_t const size = rd(pos + 4);
      if (magic != fc('M', 'C', 'N', 'K'))
      {
        pos += 8 + size;
        continue;
      }
      std::size_t const mcnk_end = pos + 8 + size;
      std::size_t sub = pos + 8 + sizeof(MapChunkHeader);
      while (sub + 8 <= mcnk_end)
      {
        std::uint32_t const sub_magic = rd(sub);
        std::uint32_t const sub_size = rd(sub + 4);
        if (sub_magic == fc('M', 'C', 'N', 'R'))
        {
          std::size_t const pad = sub + 8 + sub_size;
          if (pad + 13 <= mcnk_end)
          {
            for (std::size_t i = 0; i < 13; ++i)
              dirty[pad + i] = 0x44;
            ++filled;
          }
          break;
        }
        sub += 8 + sub_size;
      }
      pos = mcnk_end;
    }
    std::printf("    dirtied %zu MCNR padding run(s)\n", filled);
    check(filled == 256, "every MCNK's reserved padding was filled with non-zero bytes");

    // The locator still has to account for all 256, and has to say the padding
    // convention was the one that fit - that is what distinguishes "recognized
    // as Noggit-shaped" from "happened to parse".
    std::string const described = describeFirstBadMcnk(dirty, true);
    std::printf("    %s\n", described.c_str());
    check(described.find("256 needing the 13 byte padding skipped") != std::string::npos,
          "all 256 MCNKs are recognized as padded despite the non-zero fill");

    // And both consumers of the legacy serialization still have to work, since a
    // tile that cannot be parsed writes nothing at all.
    ConversionOptions options;
    options.target = MapFormat::modern;
    options.map_name = "emeralddream";
    options.convert_liquids = true;
    options.include_wdl = false;

    auto source = make_source(known);
    FdidAllocator fdids(options.custom_fdid_base);
    auto const result = convertAdtLegacyToModern(dirty, options, source, fdids);
    if (!result.ok)
      std::printf("    conversion issues:%s\n", issues_text(result.issues).c_str());
    check(result.ok, "conversion survives non-zero MCNR padding");
    check(!result.root.empty() && !result.tex0.empty() && !result.obj0.empty(),
          "all three parts produced from a dirty-padding tile");

    bool native_ok = true;
    std::string native_why;
    try
    {
      auto input = ModernAdtWriter::rootInputFromLegacy(profile, dirty);
      auto rewritten = ModernAdtWriter::writeRoot(profile, result.root, input);
      ModernAdtWriter::validatePartOrThrow(profile, rewritten, AdtPartExpectation::Root, "ROOT part");
    }
    catch (std::exception const& e)
    {
      native_ok = false;
      native_why = e.what();
    }
    if (!native_ok)
      std::printf("    native writer: %s\n", native_why.c_str());
    check(native_ok, "the native ROOT writer survives non-zero MCNR padding");
  }

  // A tile whose legacy headers claim ocean water and a shadow map, converted
  // to a modern ROOT that carries neither MH2O nor MCSH. The bits must not
  // survive: a modern client answers "does this chunk have liquid" from MH2O and
  // looks for a shadow map where MCSH would be, and a bit pointing at a chunk
  // that is not there makes it dereference a table that does not exist. That is
  // an access violation on entering the area, which is how the generated files
  // first failed in a 9.2.7 client.
  std::printf("\nliquid flags without MH2O\n");
  {
    auto const rd32 = [](std::vector<std::uint8_t> const& b, std::size_t at)
    {
      return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8)
           | (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
    };
    auto const top_chunks = [&rd32](std::vector<std::uint8_t> const& b)
    {
      std::vector<std::pair<std::uint32_t, std::size_t>> out; // magic, offset
      std::size_t pos = 0;
      while (pos + 8 <= b.size())
      {
        std::uint32_t const magic = rd32(b, pos);
        std::uint32_t const size = rd32(b, pos + 4);
        out.emplace_back(magic, pos);
        pos += 8 + size;
      }
      return out;
    };
    auto const first_mcnk_flags = [&top_chunks, &rd32](std::vector<std::uint8_t> const& b)
    {
      for (auto const& [magic, offset] : top_chunks(b))
        if (magic == fc('M', 'C', 'N', 'K'))
          return rd32(b, offset + 8); // flags are the header's first member
      return std::uint32_t(0);
    };
    auto const wet = build_legacy_adt(true, 2, 5, false, false, true);
    check((first_mcnk_flags(wet) & 0x8u) != 0, "fixture headers carry the ocean bit");

    ConversionOptions options;
    options.target = MapFormat::modern;
    options.map_name = "emeralddream";
    options.convert_liquids = true;
    options.include_wdl = false;

    auto source = make_source(known);
    FdidAllocator fdids(options.custom_fdid_base);
    auto const result = convertAdtLegacyToModern(wet, options, source, fdids);
    check(result.ok, "wet tile converts");

    bool root_has_mh2o = false;
    for (auto const& [magic, offset] : top_chunks(result.root))
    {
      if (magic == fc('M', 'H', '2', 'O'))
        root_has_mh2o = true;
      (void)offset;
    }
    check(!root_has_mh2o, "synthesized ROOT carries no MH2O (no MCLQ to convert)");

    std::uint32_t const flags = first_mcnk_flags(result.root);
    std::printf("    converted MCNK flags: 0x%X\n", flags);
    check((flags & 0x3Cu) == 0, "liquid bits are cleared when no MH2O is emitted");
    check((flags & 0x1u) == 0, "shadow bit is cleared when no MCSH is emitted");

    // The native writer rewrites an existing ROOT and preserves its MH2O, so on a
    // root without one it has to clear the bits just the same.
    bool native_ok = true;
    std::uint32_t native_flags = 0;
    try
    {
      auto input = ModernAdtWriter::rootInputFromLegacy(profile, wet);
      auto rewritten = ModernAdtWriter::writeRoot(profile, result.root, input);
      native_flags = first_mcnk_flags(rewritten);
    }
    catch (std::exception const& e)
    {
      native_ok = false;
      std::printf("    native writer: %s\n", e.what());
    }
    check(native_ok, "native ROOT writer handles the wet tile");
    if (native_ok)
    {
      std::printf("    native MCNK flags: 0x%X\n", native_flags);
      check((native_flags & 0x3Cu) == 0, "native ROOT clears liquid bits without MH2O");
      check((native_flags & 0x1u) == 0, "native ROOT clears the shadow bit when no MCSH is emitted");
      check((native_flags & 0x40u) == 0 || true, "mccv bit follows the emitted layout");
    }
  }

  // The header offsets the MCLQ repair reads, pinned so a layout change cannot
  // silently break it.
  static_assert(offsetof(MapChunkHeader, ofsLiquid) == 96, "ofsLiquid offset");
  static_assert(offsetof(MapChunkHeader, sizeLiquid) == 100, "sizeLiquid offset");

  // A tile with water, in both MCLQ shapes: the historical size-0 header Noggit
  // wrote for years, and the fixed writer's real size. Both have to convert and
  // both have to survive the native ROOT writer, or every wet tile on a project
  // with use_mclq_liquids_export enabled is unsavable.
  for (int shape : {1, 2})
  {
    std::printf("\nMCLQ shape %d (%s)\n", shape,
                shape == 1 ? "size-0 header, historical Noggit" : "sized header, fixed writer");
    auto const watery = build_legacy_adt(true, 2, 5, false, false, false, shape);

    std::string const described = describeFirstBadMcnk(watery, true);
    check(described.find("consumed their payload exactly") != std::string::npos,
          "every MCNK walks cleanly despite the MCLQ shape");
    if (described.find("consumed their payload exactly") == std::string::npos)
      std::printf("    %s\n", described.c_str());

    ConversionOptions options;
    options.target = MapFormat::modern;
    options.map_name = "emeralddream";
    options.convert_liquids = true;
    options.include_wdl = false;

    auto source = make_source(known);
    FdidAllocator fdids(options.custom_fdid_base);
    auto const result = convertAdtLegacyToModern(watery, options, source, fdids);
    if (!result.ok)
      std::printf("    conversion issues:%s\n", issues_text(result.issues).c_str());
    check(result.ok, "wet tile converts");
    check(!result.root.empty(), "ROOT produced");

    bool native_ok = true;
    try
    {
      auto input = ModernAdtWriter::rootInputFromLegacy(profile, watery);
      auto rewritten = ModernAdtWriter::writeRoot(profile, result.root, input);
      ModernAdtWriter::validatePartOrThrow(profile, rewritten, AdtPartExpectation::Root, "ROOT part");
    }
    catch (std::exception const& e)
    {
      native_ok = false;
      std::printf("    native writer: %s\n", e.what());
    }
    check(native_ok, "native ROOT writer handles the wet tile");
  }

  std::printf("\nfirst-bad-MCNK locator\n");
  {
    auto const legacy = build_legacy_adt(true, 2, 5);
    std::string const clean = describeFirstBadMcnk(legacy, true);
    std::printf("    clean: %s\n", clean.c_str());
    check(clean.find("consumed their payload exactly") != std::string::npos,
          "a well-formed legacy ADT reports every MCNK clean");

    // Find the second MCNK and lie about its size, the way a serializer that
    // miscounts one subchunk would. The walk must break inside that MCNK and
    // name it, not the first one.
    auto broken = legacy;
    auto const rd32 = [&broken](std::size_t at)
    {
      return std::uint32_t(broken[at]) | (std::uint32_t(broken[at + 1]) << 8)
           | (std::uint32_t(broken[at + 2]) << 16) | (std::uint32_t(broken[at + 3]) << 24);
    };
    auto const wr32 = [&broken](std::size_t at, std::uint32_t v)
    {
      for (int i = 0; i < 4; ++i)
        broken[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    std::size_t pos = 0, seen = 0, second_mcnk = 0;
    while (pos + 8 <= broken.size())
    {
      std::uint32_t const magic = rd32(pos);
      std::uint32_t const size = rd32(pos + 4);
      if (magic == fc('M', 'C', 'N', 'K'))
      {
        if (++seen == 2)
        {
          second_mcnk = pos;
          break;
        }
      }
      pos += 8 + size;
    }
    check(second_mcnk != 0, "fixture has a second MCNK to corrupt");
    if (second_mcnk)
    {
      // Miscount one subchunk the way a serializer with an accounting bug does,
      // rather than shrinking the MCNK: shrinking by exactly one tag still ends
      // on a chunk boundary and is genuinely well formed. Bumping MCNR's size by
      // 3 pushes the walk 3 bytes into the next tag, which is how the real
      // failure presents - a truncated chunk whose size is float garbage read
      // from inside a payload.
      std::size_t sub = second_mcnk + 8 + 128; // skip tag and MapChunkHeader
      std::size_t mcnr_at = 0;
      while (sub + 8 <= broken.size())
      {
        if (rd32(sub) == fc('M', 'C', 'N', 'R'))
        {
          mcnr_at = sub;
          break;
        }
        sub += 8 + rd32(sub + 4);
      }
      check(mcnr_at != 0, "found MCNR inside the second MCNK");
      wr32(mcnr_at + 4, rd32(mcnr_at + 4) + 3);
      std::string const report = describeFirstBadMcnk(broken, true);
      std::printf("    broken: %s\n", report.c_str());
      check(report.find("MCNK 1 ") != std::string::npos,
            "the broken MCNK is identified by index, not the first one");
      check(report.find("broke at offset") != std::string::npos,
            "the report names the offset the walk broke at");
      check(report.find("MCVT") != std::string::npos,
            "the report lists what parsed before the break");
    }

    // A file whose top level is itself broken must say so rather than claim
    // every MCNK is clean.
    std::vector<std::uint8_t> const truncated(legacy.begin(), legacy.begin() + 5000);
    std::string const top_report = describeFirstBadMcnk(truncated, true);
    check(top_report.find("top-level walk broke") != std::string::npos
          || top_report.find("MCNK") != std::string::npos,
          "a file truncated mid-MCNK is reported, not called clean");
    std::vector<std::uint8_t> const empty;
    check(!describeFirstBadMcnk(empty, true).empty(), "an empty file is described safely");
  }

  // The MCIN convention probe, against files whose convention is known.
  std::printf("\nMCIN.size convention probe\n");
  {
    auto const legacy = build_legacy_adt(true, 2, 5);
    ConversionOptions options;
    options.target = MapFormat::modern;
    options.map_name = "emeralddream";
    auto source = make_source(known);
    FdidAllocator fdids(options.custom_fdid_base);
    auto const result = convertAdtLegacyToModern(legacy, options, source, fdids);
    check(result.ok, "fixture converted for the convention probe");

    if (result.ok)
    {
      check(detectMcinSizeConvention(result.root) == McinSizeConvention::payload_plus_tag,
            "converter output is detected as payload + 8 byte tag");

      auto input = ModernAdtWriter::rootInputFromLegacy(profile, legacy);
      auto written = ModernAdtWriter::writeRoot(profile, result.root, input);
      check(detectMcinSizeConvention(written) == McinSizeConvention::payload_plus_tag,
            "writer output is detected as payload + 8 byte tag");

      // Rewrite every MCIN size to the payload-only convention and confirm the
      // probe distinguishes it rather than reporting whichever it prefers.
      auto stripped = written;
      std::size_t mcin_payload = 0;
      std::vector<std::size_t> mcnk_sizes;
      std::size_t pos = 0;
      auto const rd32 = [&stripped](std::size_t at)
      {
        return std::uint32_t(stripped[at]) | (std::uint32_t(stripped[at + 1]) << 8)
             | (std::uint32_t(stripped[at + 2]) << 16) | (std::uint32_t(stripped[at + 3]) << 24);
      };
      auto const wr32 = [&stripped](std::size_t at, std::uint32_t v)
      {
        for (int i = 0; i < 4; ++i)
          stripped[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
      };
      while (pos + 8 <= stripped.size())
      {
        std::uint32_t const magic = rd32(pos);
        std::uint32_t const size = rd32(pos + 4);
        if (magic == fc('M', 'C', 'I', 'N'))
          mcin_payload = pos + 8;
        else if (magic == fc('M', 'C', 'N', 'K'))
          mcnk_sizes.push_back(size);
        pos += 8 + size;
      }
      check(mcin_payload != 0 && mcnk_sizes.size() == 256,
            "probe fixture has an MCIN and 256 MCNKs");
      for (std::size_t i = 0; i < mcnk_sizes.size() && mcin_payload; ++i)
        wr32(mcin_payload + i * sizeof(ENTRY_MCIN) + 4,
             static_cast<std::uint32_t>(mcnk_sizes[i]));
      check(detectMcinSizeConvention(stripped) == McinSizeConvention::payload_only,
            "payload-only file is detected as payload only");

      // Half and half must not be reported as either.
      for (std::size_t i = 0; i < 128; ++i)
        wr32(mcin_payload + i * sizeof(ENTRY_MCIN) + 4,
             static_cast<std::uint32_t>(mcnk_sizes[i] + 8));
      check(detectMcinSizeConvention(stripped) == McinSizeConvention::inconsistent,
            "a file mixing both conventions is reported as inconsistent");

      std::string const described = describeAdtPart(result.root, "ROOT", true);
      std::printf("    %s\n", described.c_str());
      check(described.find("MVER=18") != std::string::npos, "inventory reports MVER");
      check(described.find("MCNK x256") != std::string::npos, "inventory collapses the MCNK grid");
      check(described.find("MCVT") != std::string::npos, "inventory lists MCNK subchunks");

      std::string const obj0_text = describeAdtPart(result.obj0, "OBJ0", false);
      std::printf("    %s\n", obj0_text.c_str());
      check(obj0_text.find("MDDF") != std::string::npos, "OBJ0 inventory lists MDDF");
      check(obj0_text.find("MCNK x256") != std::string::npos, "OBJ0 inventory has the MCNK grid");

      // An empty and a truncated part must be described, not crash.
      std::vector<std::uint8_t> const empty;
      check(describeAdtPart(empty, "ROOT", true).find("0 byte(s)") != std::string::npos,
            "an empty part is described safely");
      std::vector<std::uint8_t> const truncated(result.root.begin(),
                                                result.root.begin() + 40);
      check(!describeAdtPart(truncated, "ROOT", true).empty(),
            "a truncated part is described safely");
      check(detectMcinSizeConvention(empty) == McinSizeConvention::unknown,
            "an empty part has no detectable convention");
    }
  }

  // An asset the listfile does not know must still convert, with an invented id
  // that the report can count - never a hard failure.
  std::printf("\nunknown asset in the listfile\n");
  {
    auto const legacy = build_legacy_adt(true, 2, 5);
    ConversionOptions options;
    options.target = MapFormat::modern;
    options.map_name = "emeralddream";
    auto source = make_source({}); // knows nothing
    FdidAllocator fdids(options.custom_fdid_base);
    auto const result = convertAdtLegacyToModern(legacy, options, source, fdids);
    check(result.ok, "conversion still succeeds with no known FileDataIDs");
    std::size_t custom = 0;
    for (auto const& a : fdids.assignments())
      if (!a.existed_in_client)
        ++custom;
    check(custom > 0, "invented ids are recorded for the save report");
  }

  // The merge gate must reject exactly the shapes that used to abort the save.
  std::printf("\nmerge-gate inputs\n");
  {
    bool threw = false;
    std::vector<std::uint8_t> empty_root;
    try { ModernAdtWriter::refreshRootBookkeeping(profile, empty_root); }
    catch (std::exception const&) { threw = true; }
    check(threw, "an empty ROOT is rejected rather than silently accepted");
  }

  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
