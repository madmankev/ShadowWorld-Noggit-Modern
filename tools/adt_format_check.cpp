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
  //! `mcnr_padding` toggles the 13 zero bytes Noggit writes after MCNR. Real
  //! retail files do not have them, so both shapes have to convert.
  std::vector<std::uint8_t> build_legacy_adt(bool mcnr_padding,
                                             std::size_t layers,
                                             std::size_t doodads)
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
      std::size_t const header_slot = w.position();
      w.raw(&h, sizeof(h));

      // MCVT: 9*9 + 8*8 = 145 heights
      h.ofsHeight = static_cast<std::uint32_t>(w.position() - chunk_start);
      w.tag(fc('M', 'C', 'V', 'T')); w.u32(145 * 4);
      for (int v = 0; v < 145; ++v)
        w.f32(static_cast<float>(v));

      // MCNR: 144 normals, then Noggit's 13 unknown bytes
      h.ofsNormal = static_cast<std::uint32_t>(w.position() - chunk_start);
      w.tag(fc('M', 'C', 'N', 'R')); w.u32(144 * 3);
      w.zeros(144 * 3);
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

      // MCAL: 64x64 uncompressed bytes per non-first layer
      std::uint32_t const mcal_size =
        static_cast<std::uint32_t>(layers > 1 ? (layers - 1) * 4096 : 0);
      h.ofsAlpha = static_cast<std::uint32_t>(w.position() - chunk_start);
      h.sizeAlpha = mcal_size;
      w.tag(fc('M', 'C', 'A', 'L')); w.u32(mcal_size);
      w.zeros(mcal_size);

      // MCRF: nDoodadRefs indices into MDDF, then nMapObjRefs into MODF.
      // Chunks 0..doodads-1 each reference one doodad; chunk `doodads`
      // references the entry the converter has to drop.
      std::uint32_t n_refs = 0;
      if (doodads && i <= doodads)
        n_refs = 1;
      h.nDoodadRefs = n_refs;
      h.nMapObjRefs = 0;
      w.tag(fc('M', 'C', 'R', 'F')); w.u32(n_refs * 4);
      if (n_refs)
        w.u32(static_cast<std::uint32_t>(i)); // chunk i -> doodad i (or the dropped one)

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
  };

  for (auto const& v : {variant{"Noggit-serialized (13 padding bytes), 2 layers, 5 doodads",
                                true, 2, 5},
                        variant{"Noggit-serialized, 1 layer, 0 doodads", true, 1, 0},
                        variant{"retail-shaped (no padding), 2 layers, 5 doodads",
                                false, 2, 5}})
  {
    std::printf("\n%s\n", v.name);
    auto const legacy = build_legacy_adt(v.padding, v.layers, v.doodads);
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
