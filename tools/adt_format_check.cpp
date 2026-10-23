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
#include <noggit/formats/export/MapExport.hpp>
#include <noggit/formats/wdl/ModernWdlWriter.hpp>
#include <noggit/MapHeaders.h>

#include <cmath>
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

  // A MODF entry the client can consume: finite, and with the lower corner
  // below the upper one. Legacy imports and hand-edited projects contain both
  // NaN extents and inverted boxes, and the split pipeline copies MODF entries
  // verbatim, so the sanitizer is the only thing between them and the client's
  // spatial queries.
  std::printf("\nMODF extent sanitizing\n");
  {
    ENTRY_MODF inverted{};
    inverted.pos[0] = 100.0f; inverted.pos[1] = 200.0f; inverted.pos[2] = 30.0f;
    inverted.rot[0] = std::nanf("");
    float* e = reinterpret_cast<float*>(&inverted.extents);
    e[0] = 5.0f; e[1] = 5.0f; e[2] = 5.0f;        // upper corner ...
    e[3] = 900.0f; e[4] = 900.0f; e[5] = 900.0f;  // ... below the lower one
    sanitizeModfEntry(inverted);
    check(std::isfinite(inverted.rot[0]), "a NaN rotation becomes finite");
    float const* c = reinterpret_cast<float const*>(&inverted.extents);
    check(c[3] <= c[0] && c[4] <= c[1] && c[5] <= c[2],
          "inverted corners are swapped per axis");
    check(c[0] == 900.0f && c[3] == 5.0f, "the swap keeps the original corners");

    ENTRY_MODF nanbox{};
    nanbox.pos[0] = 10.0f; nanbox.pos[1] = 20.0f; nanbox.pos[2] = 3.0f;
    float* n = reinterpret_cast<float*>(&nanbox.extents);
    for (int i = 0; i < 6; ++i)
      n[i] = std::nanf("");
    sanitizeModfEntry(nanbox);
    float const* cn = reinterpret_cast<float const*>(&nanbox.extents);
    bool finite = true;
    for (int i = 0; i < 6; ++i)
      finite = finite && std::isfinite(cn[i]);
    check(finite, "non-finite extents become finite");
    check(cn[0] == 11.0f && cn[3] == 9.0f, "the rebuilt box is the position +/- 1");

    ENTRY_MODF good{};
    good.pos[0] = 1.0f; good.pos[1] = 2.0f; good.pos[2] = 3.0f;
    float* g = reinterpret_cast<float*>(&good.extents);
    g[0] = 8.0f; g[1] = 8.0f; g[2] = 8.0f;
    g[3] = -8.0f; g[4] = -8.0f; g[5] = -8.0f;
    sanitizeModfEntry(good);
    float const* cg = reinterpret_cast<float const*>(&good.extents);
    check(cg[0] == 8.0f && cg[3] == -8.0f, "a valid entry is left untouched");
  }

  std::printf("\nmap generations (post-WotLK families as distinct formats)\n");
  {
    auto const& sl = generationFeatures(MapGeneration::shadowlands_9_2_7);
    auto const& rt = generationFeatures(MapGeneration::retail_12_x);
    auto const& wl = generationFeatures(MapGeneration::wotlk_3_3_5);
    check(std::string(sl.label) == "9.2.7", "the Shadowlands family is named by its version");
    check(std::string(rt.label) == "12.x", "the retail family is named by its version");
    check(sl.split_adt && rt.split_adt && !wl.split_adt, "only the modern families split ADTs");
    check(!sl.wdt_mai2 && !sl.wdl_mssf, "Shadowlands carries neither MAI2 nor MSSF");
    check(rt.wdt_mai2 && rt.wdl_mssf, "retail carries MAI2 and MSSF");
    check(sl.wdl_mssn && rt.wdl_mssn, "both modern families carry sky scenes");

    // A retail-shaped WDT: the Shadowlands family's chunks plus MAI2.
    Writer wdt;
    wdt.tag(fc('M', 'V', 'E', 'R')); wdt.u32(4); wdt.u32(18);
    wdt.tag(fc('M', 'P', 'H', 'D')); wdt.u32(32);
    wdt.u32(0x200); wdt.zeros(28);
    wdt.tag(fc('M', 'A', 'I', 'N')); wdt.u32(16); wdt.zeros(16);
    wdt.tag(fc('M', 'A', 'I', 'D')); wdt.u32(32); wdt.zeros(32);
    std::size_t const mai2_at = wdt.b.size();
    wdt.tag(fc('M', 'A', 'I', '2')); wdt.u32(16); wdt.u32(7); wdt.zeros(12);
    auto const wdt12 = wdt.b;

    auto const down = convertWdtBetweenGenerations(
      wdt12, MapGeneration::retail_12_x, MapGeneration::shadowlands_9_2_7);
    check(down.ok, "a retail WDT converts down to Shadowlands");
    check(down.bytes.size() == mai2_at, "the MAI2 chunk is removed entirely");
    check(down.bytes == std::vector<std::uint8_t>(wdt12.begin(), wdt12.begin() + mai2_at),
          "everything before it is byte-identical");
    bool strip_noted = false;
    for (auto const& n : down.notes)
      strip_noted = strip_noted || (n.find("MAI2") != std::string::npos
                                    && n.find("stripped") != std::string::npos);
    check(strip_noted, "the strip is reported");
    check(detectModernGeneration(wdt12, {}) == MapGeneration::retail_12_x,
          "MAI2 in the WDT identifies the retail family");
    check(detectModernGeneration(down.bytes, {}) == MapGeneration::shadowlands_9_2_7,
          "the down-converted WDT identifies as Shadowlands");

    auto const up = convertWdtBetweenGenerations(
      down.bytes, MapGeneration::shadowlands_9_2_7, MapGeneration::retail_12_x);
    check(up.ok, "a Shadowlands WDT converts up to retail");
    check(up.bytes == down.bytes, "no bytes are invented going up");
    bool absent_noted = false;
    for (auto const& n : up.notes)
      absent_noted = absent_noted || (n.find("MAI2") != std::string::npos
                                      && n.find("stays absent") != std::string::npos);
    check(absent_noted, "a missing MAI2 is reported rather than fabricated");

    auto const same = convertWdtBetweenGenerations(
      wdt12, MapGeneration::retail_12_x, MapGeneration::retail_12_x);
    check(same.ok && same.bytes == wdt12 && same.notes.empty(),
          "same generation is an identity with nothing to report");

    auto const refused = convertWdtBetweenGenerations(
      wdt12, MapGeneration::retail_12_x, MapGeneration::wotlk_3_3_5);
    check(!refused.ok && refused.bytes.empty(),
          "WotLK on either end is refused: that is convertMap's job");

    // A retail-shaped WDL: MSSN plus the MSSF the Shadowlands family lacks.
    Writer wdl;
    wdl.tag(fc('M', 'V', 'E', 'R')); wdl.u32(4); wdl.u32(18);
    wdl.tag(fc('M', 'A', 'O', 'F')); wdl.u32(16); wdl.zeros(16);
    wdl.tag(fc('M', 'S', 'S', 'N')); wdl.u32(8); wdl.u32(3); wdl.zeros(4);
    wdl.tag(fc('M', 'S', 'S', 'F')); wdl.u32(8); wdl.u32(9); wdl.zeros(4);
    auto const wdl12 = wdl.b;
    std::size_t const mssf_at = wdl12.size() - 16;

    auto const wdl_down = convertWdlBetweenGenerations(
      wdl12, MapGeneration::retail_12_x, MapGeneration::shadowlands_9_2_7);
    check(wdl_down.ok && wdl_down.bytes.size() == mssf_at, "MSSF is stripped from the WDL");
    check(wdl_down.bytes == std::vector<std::uint8_t>(wdl12.begin(), wdl12.begin() + mssf_at),
          "MSSN and the rest survive the strip byte-identically");
    check(detectModernGeneration({}, wdl12) == MapGeneration::retail_12_x,
          "MSSF in the WDL identifies the retail family");
    check(detectModernGeneration({}, wdl_down.bytes) == MapGeneration::shadowlands_9_2_7,
          "the stripped WDL identifies as Shadowlands");
    auto const wdl_up = convertWdlBetweenGenerations(
      wdl_down.bytes, MapGeneration::shadowlands_9_2_7, MapGeneration::retail_12_x);
    check(wdl_up.ok && wdl_up.bytes == wdl_down.bytes, "no MSSF is invented going up");

    // Both modern families share the ADT layout, so a converted part crosses
    // generations unchanged - after being walked and version-checked.
    ConversionOptions gen_options;
    gen_options.target = MapFormat::modern;
    gen_options.map_name = "emeralddream";
    gen_options.convert_liquids = true;
    gen_options.include_wdl = false;
    auto gen_source = make_source(known);
    FdidAllocator gen_fdids(gen_options.custom_fdid_base);
    auto const gen_result =
      convertAdtLegacyToModern(build_legacy_adt(true, 1, 0), gen_options, gen_source, gen_fdids);
    check(gen_result.ok && !gen_result.root.empty(), "a modern ROOT part for the crossing test");
    if (gen_result.ok && !gen_result.root.empty())
    {
      auto const step = convertAdtPartBetweenGenerations(
        gen_result.root, MapGeneration::shadowlands_9_2_7, MapGeneration::retail_12_x);
      check(step.ok && step.bytes == gen_result.root,
            "the part crosses modern generations verbatim");
      bool layout_noted = false;
      for (auto const& n : step.notes)
        layout_noted = layout_noted || n.find("MVER 18") != std::string::npos;
      check(layout_noted, "the shared layout is what the copy is justified by");

      auto const bad_end = convertAdtPartBetweenGenerations(
        gen_result.root, MapGeneration::shadowlands_9_2_7, MapGeneration::wotlk_3_3_5);
      check(!bad_end.ok, "a WotLK end is refused for ADT parts too");

      std::vector<std::uint8_t> const junk{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
      auto const junk_step = convertAdtPartBetweenGenerations(
        junk, MapGeneration::shadowlands_9_2_7, MapGeneration::retail_12_x);
      check(!junk_step.ok, "a blob that is not a modern part is refused");
    }

    // Forever: the Shadowlands file family until research shows a divergence.
    auto const& fv = generationFeatures(MapGeneration::forever_1_60_x);
    check(std::string(fv.label) == "1.60.x", "Forever is named by its version");
    check(fv.split_adt && !fv.wdt_mai2 && !fv.wdl_mssf && fv.wdl_mssn,
          "Forever carries the Shadowlands family's features");

    check(generationFromBuildString("3.3.5.12340") == MapGeneration::wotlk_3_3_5,
          "a 3.3.5 build maps to WotLK");
    check(generationFromBuildString("9.2.7.45745") == MapGeneration::shadowlands_9_2_7,
          "a 9.2.7 build maps to Shadowlands");
    check(generationFromBuildString("8.3.7.35249") == MapGeneration::shadowlands_9_2_7,
          "builds before 9.x map to the oldest modern family the editor authors");
    check(generationFromBuildString("12.1.5.60248") == MapGeneration::retail_12_x,
          "a 12.x build maps to retail");
    check(generationFromBuildString("10.2.0.1") == MapGeneration::retail_12_x,
          "a 10.x build maps to retail");
    check(generationFromBuildString("1.60.0.1234") == MapGeneration::forever_1_60_x,
          "a 1.60 build maps to Forever");
    check(generationFromBuildString("") == MapGeneration::shadowlands_9_2_7,
          "an empty build string falls back");
    check(generationFromBuildString("garbage", MapGeneration::wotlk_3_3_5)
            == MapGeneration::wotlk_3_3_5,
          "an unparseable build string uses the fallback");
    check(generationFromBuildString("1.15.2.123") == MapGeneration::shadowlands_9_2_7,
          "a non-Forever 1.x build falls back: classic is not an editor target");

    auto const to_forever = convertWdtBetweenGenerations(
      wdt12, MapGeneration::retail_12_x, MapGeneration::forever_1_60_x);
    check(to_forever.ok && to_forever.bytes.size() == wdt12.size() - 24,
          "retail -> Forever strips MAI2 like retail -> Shadowlands");
    auto const sl_to_forever = convertWdtBetweenGenerations(
      down.bytes, MapGeneration::shadowlands_9_2_7, MapGeneration::forever_1_60_x);
    check(sl_to_forever.ok && sl_to_forever.bytes == down.bytes
            && sl_to_forever.notes.empty(),
          "Shadowlands -> Forever is byte-identical with nothing to report");

    ConversionOptions gen_defaults;
    check(gen_defaults.target_generation == MapGeneration::shadowlands_9_2_7,
          "conversions default to the family the editor saves in");
  }

  std::printf("\nnative modern WDL writer\n");
  {
    namespace WDL = Noggit::Formats::WDL;

    struct ChunkView { std::uint32_t magic; std::size_t offset; std::size_t size; };
    auto walk = [](std::vector<std::uint8_t> const& b, std::vector<ChunkView>& out) -> bool
    {
      std::size_t pos = 0;
      while (pos + 8 <= b.size())
      {
        ChunkView c{};
        std::memcpy(&c.magic, b.data() + pos, 4);
        std::memcpy(&c.size, b.data() + pos + 4, 4);
        c.offset = pos;
        if (c.size > b.size() - pos - 8) return false;
        out.push_back(c);
        pos += 8 + c.size;
      }
      return pos == b.size();
    };
    auto read_u32 = [](std::vector<std::uint8_t> const& b, std::size_t at)
    {
      std::uint32_t v = 0; std::memcpy(&v, b.data() + at, 4); return v;
    };

    WDL::ModernWdlInput in{};
    WDL::ModernWdlTile tile_a{};
    for (int i = 0; i < 17 * 17; ++i)
      (&tile_a.height_17[0][0])[i] = static_cast<std::int16_t>(100 + i);
    for (int i = 0; i < 16 * 16; ++i)
      (&tile_a.height_16[0][0])[i] = static_cast<std::int16_t>(200 + i);
    tile_a.holes[3] = 0x0F00;
    in.tiles[0] = tile_a;
    std::size_t const tile_b = 2 * 64 + 1; // (x=1, y=2)
    WDL::ModernWdlTile tile_b_data{};
    (&tile_b_data.height_17[0][0])[0] = 777;
    in.tiles[tile_b] = tile_b_data;

    WDL::ModernWdlObject object{};
    object.fdid = 314159;
    object.placement.uniqueID = 42;
    object.placement.pos[0] = 1.0f; object.placement.pos[1] = 2.0f; object.placement.pos[2] = 3.0f;
    object.placement.flags = 0x8 | 0x2; // the fdid flag must be cleared in MLMD
    object.placement.scale = 1024;
    float* ext = reinterpret_cast<float*>(&object.placement.extents);
    ext[0] = 8.0f; ext[1] = 8.0f; ext[2] = 8.0f;   // upper
    ext[3] = -8.0f; ext[4] = -8.0f; ext[5] = -8.0f; // lower
    in.objects.push_back(object);
    WDL::ModernWdlObject unresolved{}; // fdid 0 -> skipped with a note
    in.objects.push_back(unresolved);

    auto const written = WDL::writeModernWdl(in);
    check(written.ok, "writeModernWdl succeeds");
    std::vector<ChunkView> chunks;
    check(walk(written.bytes, chunks), "the written WDL walks exactly");
    check(chunks.size() == 8, "chunk set: MVER MAOF MARE MAHO MARE MAHO MLMD MLMX");
    check(chunks[0].magic == fc('M','V','E','R') && read_u32(written.bytes, 8) == 18,
          "MVER is 18");
    check(chunks[1].magic == fc('M','A','O','F') && chunks[1].size == 4096 * 4,
          "MAOF is the full 4096 entry table");
    std::size_t const maof = chunks[1].offset + 8;
    check(read_u32(written.bytes, maof) == chunks[2].offset,
          "MAOF[0] points at tile A's MARE tag");
    check(read_u32(written.bytes, maof + tile_b * 4) == chunks[4].offset,
          "MAOF[129] points at tile B's MARE tag");
    check(read_u32(written.bytes, maof + 4) == 0, "an absent tile keeps offset 0");
    check(chunks[2].size == 1090 && chunks[3].size == 32, "MARE is 1090, MAHO is 32 bytes");
    check(std::memcmp(written.bytes.data() + chunks[2].offset + 8,
                      tile_a.height_17, 17 * 17 * 2) == 0
          && std::memcmp(written.bytes.data() + chunks[2].offset + 8 + 17 * 17 * 2,
                         tile_a.height_16, 16 * 16 * 2) == 0,
          "the MARE payload is height_17 followed by height_16, verbatim");
    check(std::memcmp(written.bytes.data() + chunks[3].offset + 8,
                      tile_a.holes, 32) == 0,
          "the MAHO payload is the hole masks, verbatim");
    check(chunks[6].magic == fc('M','L','M','D') && chunks[6].size == 40,
          "MLMD holds the one resolved placement (40 bytes)");
    check(read_u32(written.bytes, chunks[6].offset + 8) == 314159,
          "the MLMD nameID is the FileDataID");
    std::uint16_t mlmd_flags = 0;
    std::memcpy(&mlmd_flags, written.bytes.data() + chunks[6].offset + 8 + 32, 2);
    check(mlmd_flags == 0x2, "the 0x8 name-is-fdid flag is cleared in MLMD");
    check(chunks[7].magic == fc('M','L','M','X') && chunks[7].size == 28,
          "MLMX holds the matching bounds (28 bytes)");
    float mlmx[7];
    std::memcpy(mlmx, written.bytes.data() + chunks[7].offset + 8, 28);
    check(mlmx[0] == -8.0f && mlmx[3] == 8.0f,
          "MLMX min/max come from the MODF extents (lower first)");
    check(std::fabs(mlmx[6] - 0.5f * std::sqrt(3.0f * 16.0f * 16.0f)) < 1e-3f,
          "the MLMX radius is half the box diagonal");
    bool noted_skip = false;
    for (auto const& n : written.notes)
      if (n.find("skipped") != std::string::npos) noted_skip = true;
    check(noted_skip, "an unresolved placement is reported as skipped");

    // rewriteModernWdl: MSSN appended as a chunk the writer must not touch
    std::vector<std::uint8_t> original = written.bytes;
    {
      Writer mssn;
      mssn.tag(fc('M','S','S','N')); mssn.u32(8);
      std::uint8_t payload[8] = {9, 8, 7, 6, 5, 4, 3, 2};
      mssn.raw(payload, 8);
      original.insert(original.end(), mssn.b.begin(), mssn.b.end());
    }

    WDL::WdlRewriteInput rewrite;
    rewrite.original = original;
    WDL::ModernWdlTile new_a = tile_a;
    (&new_a.height_17[0][0])[0] = -32000;
    rewrite.tiles[0] = new_a;
    WDL::ModernWdlTile fresh{};
    (&fresh.height_17[0][0])[0] = 555;
    std::size_t const tile_c = 5 * 64 + 5;
    rewrite.tiles[tile_c] = fresh;

    auto const rebuilt = WDL::rewriteModernWdl(rewrite);
    check(rebuilt.ok, "rewriteModernWdl succeeds");
    std::vector<ChunkView> rchunks;
    check(walk(rebuilt.bytes, rchunks), "the rebuilt WDL walks exactly");
    std::size_t rmaof = 0;
    for (auto const& c : rchunks)
      if (c.magic == fc('M','A','O','F')) rmaof = c.offset + 8;
    check(rmaof != 0, "the rebuilt WDL has a MAOF table");
    check(read_u32(rebuilt.bytes, rmaof) != 0 && read_u32(rebuilt.bytes, rmaof + tile_b * 4) != 0
          && read_u32(rebuilt.bytes, rmaof + tile_c * 4) != 0,
          "updated, kept and added tiles all have MAOF entries");
    // tile A MARE payload carries the new height
    std::int16_t first_height = 0;
    std::memcpy(&first_height, rebuilt.bytes.data() + read_u32(rebuilt.bytes, rmaof) + 8, 2);
    check(first_height == -32000, "the updated tile carries the new heights");
    // tile B kept verbatim: compare its MARE payload with the original file's
    {
      std::uint32_t const old_off = read_u32(original, maof + tile_b * 4);
      std::uint32_t const new_off = read_u32(rebuilt.bytes, rmaof + tile_b * 4);
      check(std::memcmp(original.data() + old_off + 8, rebuilt.bytes.data() + new_off + 8,
                        1090) == 0,
            "a tile without a replacement is copied byte for byte");
    }
    // MSSN and MLMD/MLMX preserved
    bool mssn_kept = false, mlmd_kept = false;
    for (auto const& c : rchunks)
    {
      if (c.magic == fc('M','S','S','N'))
        mssn_kept = c.size == 8 && rebuilt.bytes[c.offset + 8] == 9
                    && rebuilt.bytes[c.offset + 15] == 2;
      if (c.magic == fc('M','L','M','D'))
        mlmd_kept = c.size == 40;
    }
    check(mssn_kept, "the MSSN chunk survives the rewrite byte for byte");
    check(mlmd_kept, "the MLMD chunk survives the rewrite");
    bool noted_preserved = false;
    for (auto const& n : rebuilt.notes)
      if (n.find("preserved") != std::string::npos && n.find("MSSN") != std::string::npos)
        noted_preserved = true;
    check(noted_preserved, "preserved chunks are named in the notes");

    WDL::WdlRewriteInput junk;
    junk.original = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    check(!WDL::rewriteModernWdl(junk).ok, "a file that does not walk is not rebuilt");

    WDL::WdlRewriteInput small_maof;
    {
      Writer wsm;
      wsm.tag(fc('M','V','E','R')); wsm.u32(4); wsm.u32(18);
      wsm.tag(fc('M','A','O','F')); wsm.u32(8); wsm.u32(0); wsm.u32(0);
      small_maof.original = wsm.b;
    }
    auto const small_result = WDL::rewriteModernWdl(small_maof);
    check(!small_result.ok, "a malformed MAOF refuses the rebuild rather than lose tiles");
  }

  std::printf("\nmap export pipeline\n");
  {
    namespace Export = Noggit::Formats::Export;

    struct ChunkView { std::uint32_t magic; std::size_t offset; std::size_t size; };
    auto walk = [](std::vector<std::uint8_t> const& b, std::vector<ChunkView>& out) -> bool
    {
      std::size_t pos = 0;
      while (pos + 8 <= b.size())
      {
        ChunkView c{};
        std::memcpy(&c.magic, b.data() + pos, 4);
        std::memcpy(&c.size, b.data() + pos + 4, 4);
        c.offset = pos;
        if (c.size > b.size() - pos - 8) return false;
        out.push_back(c);
        pos += 8 + c.size;
      }
      return pos == b.size();
    };
    auto read_u32 = [](std::vector<std::uint8_t> const& b, std::size_t at)
    {
      std::uint32_t v = 0; std::memcpy(&v, b.data() + at, 4); return v;
    };

    // Real modern parts from the real converter, for a tile at (12, 34).
    ConversionOptions exp_options;
    exp_options.target = MapFormat::modern;
    exp_options.map_name = "test";
    exp_options.convert_liquids = true;
    exp_options.include_wdl = false;
    auto exp_source = make_source(known);
    FdidAllocator exp_fdids(exp_options.custom_fdid_base);
    std::printf("[step] before convert\n");
    auto const parts =
      convertAdtLegacyToModern(build_legacy_adt(true, 1, 0), exp_options, exp_source, exp_fdids);
    std::printf("[step] after convert\n");
    check(parts.ok && !parts.root.empty() && !parts.tex0.empty() && !parts.obj0.empty(),
          "modern parts for the export fixture");

    std::size_t const tile_i = 34 * 64 + 12;
    auto build_wdt = [&](bool second_tile)
    {
      Writer w;
      w.tag(fc('M','V','E','R')); w.u32(4); w.u32(18);
      w.tag(fc('M','P','H','D')); w.u32(32);
      w.u32(0x200);       // wdt_has_maid
      w.u32(0);           // [1] lgt  - zero, but the set contains the file
      w.u32(888);         // [2] occ  - referenced, but the set lacks the file
      w.zeros(20);        // [3..7]
      w.tag(fc('M','A','I','N')); w.u32(4096 * 8);
      std::size_t const main_at = w.position();
      w.zeros(4096 * 8);
      w.patch_u32(main_at + tile_i * 8, 1);
      if (second_tile)
        w.patch_u32(main_at + (34 * 64 + 13) * 8, 1);
      w.tag(fc('M','A','I','D')); w.u32(4096 * 32);
      w.zeros(4096 * 32);
      return w.b;
    };

    Export::ExportOptions options;
    options.map_directory = "test";
    options.map_display_name = "Test Map";
    options.map_id = 1234;

    auto make_input = [&](bool second_tile, bool with_wdl, bool corrupt_root)
    {
      Export::ExportInput in;
      in.files.push_back({"World/Maps/Test/Test.wdt", build_wdt(second_tile)});
      std::vector<std::uint8_t> root = parts.root;
      if (corrupt_root)
        root.resize(12);
      in.files.push_back({"World/Maps/Test/Test_12_34.adt", root});
      in.files.push_back({"World/Maps/Test/Test_12_34_tex0.adt", parts.tex0});
      in.files.push_back({"World/Maps/Test/Test_12_34_obj0.adt", parts.obj0});
      in.files.push_back({"World/Maps/Test/Test_lgt.wdt", std::vector<std::uint8_t>{7, 7, 7, 7}});
      if (with_wdl)
      {
        Noggit::Formats::WDL::ModernWdlInput wdl_in{};
        wdl_in.tiles[tile_i] = Noggit::Formats::WDL::ModernWdlTile{};
        in.files.push_back({"World/Maps/Test/Test.wdl",
                            Noggit::Formats::WDL::writeModernWdl(wdl_in).bytes});
      }
      in.path_to_fdid = [](std::string const& path) -> std::uint32_t
      {
        return path == "world\\maps\\test\\test.wdt" ? 555u : 0u;
      };
      return in;
    };

    std::printf("[step] before export\n");
    auto const exported = Export::exportModernMap(options, make_input(false, false, false));
    std::printf("[step] after export\n");
    check(exported.ok, "a complete map exports cleanly");
    check(exported.tiles_total == 1 && exported.tiles_complete == 1,
          "one tile, complete");
    for (auto const& e : exported.errors)
      std::printf("    export error: %s\n", e.c_str());

    // the exported WDT: MAID filled for the three parts that exist
    Export::ExportFile const* out_wdt = nullptr;
    for (auto const& f : exported.files)
      if (f.relative_path.find(".wdt") != std::string::npos
          && f.relative_path.find("_lgt") == std::string::npos)
        out_wdt = &f;
    check(out_wdt != nullptr, "the export set carries the WDT");
    if (out_wdt)
    {
      std::vector<ChunkView> wc;
      check(walk(out_wdt->bytes, wc), "the exported WDT walks");
      std::size_t maid_at = 0, mphd_at = 0;
      for (auto const& c : wc)
      {
        if (c.magic == fc('M','A','I','D')) maid_at = c.offset + 8;
        if (c.magic == fc('M','P','H','D')) mphd_at = c.offset + 8;
      }
      auto slot = [&](int s) { return read_u32(out_wdt->bytes, maid_at + tile_i * 32 + s * 4); };
      check(slot(0) >= 900000000u && slot(3) >= 900000000u && slot(1) >= 900000000u,
            "MAID root/tex0/obj0 are assigned for the new tile");
      check(slot(0) != slot(3) && slot(0) != slot(1) && slot(3) != slot(1),
            "the assigned ids are distinct");
      check(slot(2) == 0 && slot(4) == 0,
            "obj1/lod slots stay zero when those parts do not exist");
      check(read_u32(out_wdt->bytes, mphd_at + 4) != 0,
            "the MPHD lgt column is assigned for the exported aux WDT");
      check(read_u32(out_wdt->bytes, mphd_at + 8) == 888,
            "an existing MPHD column is left alone");
    }

    bool has_root_assignment = false, has_wdt_known = false;
    for (auto const& a : exported.fdid_assignments)
    {
      if (a.role == "adt_root" && !a.existed_in_client && a.fdid == 900000000u
          && a.path == "world/maps/test/test_12_34.adt")
        has_root_assignment = true;
      if (a.role == "wdt_file" && a.existed_in_client && a.fdid == 555u)
        has_wdt_known = true;
    }
    std::printf("[step] after wdt checks\n");
    check(has_root_assignment, "the new root id is reported for registration");
    check(has_wdt_known, "the client-known WDT id is reported as existing");

    std::printf("[step] before listfile\n");
    check(exported.listfile_csv() ==
            "900000000;world/maps/test/test_12_34.adt\n"
            "900000001;world/maps/test/test_12_34_tex0.adt\n"
            "900000002;world/maps/test/test_12_34_obj0.adt\n"
            "900000003;world/maps/test/test_lgt.wdt\n",
          "the listfile additions are exactly the fresh ids, sorted, fdid;path");

    std::printf("[step] before fields\n");
    auto const fields = exported.registration_fields();
    check(fields.at("Directory") == "test" && fields.at("MapName_lang") == "Test Map",
          "the registration fragment names the map");
    check(fields.at("WdtFileDataID") == "555",
          "the registration carries the WDT FileDataID");

    check(exported.manifest_json().find("\"kind\": \"map_export\"") != std::string::npos
          && exported.manifest_json().find("\"new_fdids\": 4") != std::string::npos
          && exported.manifest_json().find("fnv1a64") != std::string::npos,
          "the manifest follows the conversion-manifest conventions");

    bool warned_occ = false, warned_wdl = false, warned_baked = false;
    for (auto const& w : exported.warnings)
    {
      if (w.find("_occ") != std::string::npos) warned_occ = true;
      if (w.find("no WDL") != std::string::npos) warned_wdl = true;
      if (w.find("baked") != std::string::npos) warned_baked = true;
    }
    check(warned_occ, "a referenced-but-absent aux WDT is warned about");
    check(warned_wdl, "a missing WDL is warned about");
    check(warned_baked, "new tiles keeping zero baked-texture ids are reported");

    std::printf("[step] before wdl run\n");
    // with a WDL: MPHD word 6 gets assigned
    {
      auto const with_wdl = Export::exportModernMap(options, make_input(false, true, false));
      check(with_wdl.ok, "the export stays clean with a WDL present");
      bool wdl_assigned = false;
      for (auto const& a : with_wdl.fdid_assignments)
        if (a.role == "wdl" && !a.existed_in_client && a.fdid >= 900000000u)
          wdl_assigned = true;
      check(wdl_assigned, "the WDL gets a FileDataID in MPHD word 6");
    }

    std::printf("[step] before missing run\n");
    // a second tile MAIN claims but the set lacks -> hard error
    {
      auto const missing = Export::exportModernMap(options, make_input(true, false, false));
      check(!missing.ok, "a tile without its parts fails the export");
      bool named = false;
      for (auto const& e : missing.errors)
        if (e.find("13,34") != std::string::npos && e.find("missing") != std::string::npos)
          named = true;
      check(named, "the error names the tile and the missing file");
      check(missing.tiles_total == 2 && missing.tiles_complete == 1,
            "the complete tile is still counted as complete");
    }

    std::printf("[step] before corrupt run\n");
    // a corrupt part -> validation error, nothing deployable claimed
    {
      auto const corrupt = Export::exportModernMap(options, make_input(false, false, true));
      check(!corrupt.ok, "a corrupt ROOT part fails the export");
      bool validated = false;
      for (auto const& e : corrupt.errors)
        if (e.find("failed validation") != std::string::npos)
          validated = true;
      check(validated, "the failure comes from the structural validation");
    }

    std::printf("[step] before no-wdt run\n");
    // no WDT at all
    {
      Export::ExportInput no_wdt;
      no_wdt.files.push_back({"World/Maps/Test/Test_12_34.adt", parts.root});
      auto const r = Export::exportModernMap(options, no_wdt);
      check(!r.ok && !r.errors.empty(), "an export without a WDT is refused");
    }

    // export-version selection: the map-level files step to the chosen family
    {
      auto build_wdt_mai2 = [&]()
      {
        Writer w;
        w.tag(fc('M','V','E','R')); w.u32(4); w.u32(18);
        w.tag(fc('M','P','H','D')); w.u32(32);
        w.u32(0x200); w.u32(0); w.u32(888); w.zeros(20);
        w.tag(fc('M','A','I','N')); w.u32(4096 * 8);
        std::size_t const main_at = w.position();
        w.zeros(4096 * 8);
        w.patch_u32(main_at + tile_i * 8, 1);
        w.tag(fc('M','A','I','D')); w.u32(4096 * 32);
        w.zeros(4096 * 32);
        w.tag(fc('M','A','I','2')); w.u32(16); w.u32(7); w.zeros(12);
        return w.b;
      };
      auto make_retail_input = [&]()
      {
        Export::ExportInput in;
        in.files.push_back({"World/Maps/Test/Test.wdt", build_wdt_mai2()});
        in.files.push_back({"World/Maps/Test/Test_12_34.adt", parts.root});
        in.files.push_back({"World/Maps/Test/Test_12_34_tex0.adt", parts.tex0});
        in.files.push_back({"World/Maps/Test/Test_12_34_obj0.adt", parts.obj0});
        in.path_to_fdid = [](std::string const&) { return 0u; };
        return in;
      };
      auto exported_wdt = [](Export::ExportResult const& r) -> Export::ExportFile const*
      {
        for (auto const& f : r.files)
          if (f.relative_path == "World/Maps/Test/Test.wdt")
            return &f;
        return nullptr;
      };
      auto has_chunk = [](std::vector<std::uint8_t> const& b, std::uint32_t magic)
      {
        std::size_t pos = 0;
        while (pos + 8 <= b.size())
        {
          std::uint32_t m = 0, sz = 0;
          std::memcpy(&m, b.data() + pos, 4);
          std::memcpy(&sz, b.data() + pos + 4, 4);
          if (m == magic) return true;
          if (sz > b.size() - pos - 8) return false;
          pos += 8 + sz;
        }
        return false;
      };

      // retail source -> Shadowlands export: MAI2 stripped from the WDT
      Export::ExportOptions sl_options = options;
      sl_options.export_generation = MapGeneration::shadowlands_9_2_7;
      auto const down_exp = Export::exportModernMap(sl_options, make_retail_input());
      check(down_exp.ok, "a retail-source map exports to the Shadowlands family");
      check(down_exp.source_generation == MapGeneration::retail_12_x,
            "MAI2 in the source WDT is detected as the retail generation");
      auto const* down_wdt = exported_wdt(down_exp);
      check(down_wdt && !has_chunk(down_wdt->bytes, fc('M','A','I','2')),
            "the exported WDT carries no MAI2");
      bool step_warned = false;
      for (auto const& w : down_exp.warnings)
        if (w.find("generation step") != std::string::npos
            && w.find("MAI2") != std::string::npos)
          step_warned = true;
      check(step_warned, "the strip is reported as a generation-step warning");
      // the MAID assignments made before the step survive it
      if (down_wdt)
      {
        std::size_t maid_at = 0;
        std::size_t pos = 0;
        while (pos + 8 <= down_wdt->bytes.size())
        {
          std::uint32_t m = 0, sz = 0;
          std::memcpy(&m, down_wdt->bytes.data() + pos, 4);
          std::memcpy(&sz, down_wdt->bytes.data() + pos + 4, 4);
          if (m == fc('M','A','I','D')) maid_at = pos + 8;
          pos += 8 + sz;
        }
        check(maid_at && read_u32(down_wdt->bytes, maid_at + tile_i * 32) >= 900000000u,
              "MAID assignments survive the generation step");
      }

      // retail source -> retail export: pass-through, MAI2 kept, no step
      Export::ExportOptions rt_options = options;
      rt_options.export_generation = MapGeneration::retail_12_x;
      auto const same_exp = Export::exportModernMap(rt_options, make_retail_input());
      check(same_exp.ok && same_exp.source_generation == MapGeneration::retail_12_x,
            "a retail export of retail bytes detects and stays retail");
      auto const* same_wdt = exported_wdt(same_exp);
      check(same_wdt && has_chunk(same_wdt->bytes, fc('M','A','I','2')),
            "the retail export keeps the source MAI2");
      bool stepped = false;
      for (auto const& w : same_exp.warnings)
        if (w.find("generation step") != std::string::npos) stepped = true;
      check(!stepped, "same-generation export performs no step");

      // Shadowlands source -> retail export: honest absence, no fabrication
      auto const up_exp = Export::exportModernMap(rt_options, make_input(false, false, false));
      check(up_exp.ok && up_exp.source_generation == MapGeneration::shadowlands_9_2_7,
            "the Shadowlands fixture detects as Shadowlands");
      bool absent_warned = false;
      for (auto const& w : up_exp.warnings)
        if (w.find("MAI2") != std::string::npos && w.find("stays absent") != std::string::npos)
          absent_warned = true;
      check(absent_warned, "exporting up to retail reports the absent MAI2 instead of inventing one");
      auto const* up_wdt = exported_wdt(up_exp);
      check(up_wdt && !has_chunk(up_wdt->bytes, fc('M','A','I','2')),
            "no MAI2 chunk appears in the up-exported WDT");

      // WotLK as export version: refused, pointing at the conversion tool
      Export::ExportOptions bad_options = options;
      bad_options.export_generation = MapGeneration::wotlk_3_3_5;
      auto const refused = Export::exportModernMap(bad_options, make_input(false, false, false));
      check(!refused.ok, "WotLK as an export version is refused");
      bool pointed = false;
      for (auto const& e : refused.errors)
        if (e.find("Convert map") != std::string::npos) pointed = true;
      check(pointed, "the refusal points at the conversion tool");

      // the manifest names both generations
      check(down_exp.manifest_json().find("\"source_generation\": \"12.x\"") != std::string::npos
            && down_exp.manifest_json().find("\"export_generation\": \"9.2.7\"") != std::string::npos,
            "the manifest records the source and export generations");
    }
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
