// Per-client map-level save spec.
//
// Encodes the WDT/WDL shapes each modern client actually carries and proves
// the byte-preserving updaters keep every client-specific chunk:
//   - Shadowlands 9.2.7 WDT: MPHD flags 0x80/0x100/0x200, seven fdid slots, MAID.
//   - Forever 1.60.x WDT: the Shadowlands shape plus MAI2 (field evidence: a
//     1.60.1.70685 client's own Kalimdor.wdt carries MAI2).
//   - Modern WDL (SL/Retail/Forever): MAOF -> per-tile MARE(+MAHO) plus object
//     LOD (MLDD/MLDX/MLMD/MLMX/MLDF) and sky-scene (MSSN/MSSC/MSSO/MSSF)
//     chunks - field evidence: the Forever client's Kalimdor.wdl carries
//     MLDD/MLDF/MLDX.
// The from-scratch legacy writer reproduces none of these; these checks are
// the regression gate for routing modern saves through the preserving path.
#include <noggit/formats/RetailWorldTables.hpp>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace Noggit::Formats::RetailWorldTables;

static int checks = 0;
static int failures = 0;

static void check_msg(bool ok, std::string const& what)
{
  ++checks;
  if (!ok)
  {
    ++failures;
    std::cout << "  FAIL " << what << "\n";
  }
}

static void put(Bytes& b, std::size_t p, std::uint32_t n)
{
  std::memcpy(b.data() + p, &n, 4);
}

// Appends a chunk whose payload is filled with a recognizable pattern, so any
// accidental overwrite shows up in byte comparisons.
static std::size_t add(Bytes& b, char const* tag, std::size_t size)
{
  auto p = b.size();
  b.resize(p + 8 + size);
  for (unsigned i = 0; i < 4; ++i)
    b[p + i] = tag[3 - i];
  put(b, p + 4, static_cast<std::uint32_t>(size));
  for (std::size_t i = 0; i < size; ++i)
    b[p + 8 + i] = static_cast<char>((tag[0] + i) & 0x7f);
  return p + 8;
}

struct WdtLayout
{
  std::size_t mphd = 0;
  std::size_t main = 0;
  std::size_t maid = 0;
  std::size_t mai2 = 0;
};

static Bytes makeModernWdt(std::uint32_t flags, bool with_maid, bool with_mai2,
                           WdtLayout& out)
{
  Bytes wdt;
  put(wdt, add(wdt, "MVER", 4), 18);
  out.mphd = add(wdt, "MPHD", 32);
  put(wdt, out.mphd, flags);
  // MPHD file ids: lgt, occ, fogs, mpv, tex, wdl, pd4.
  for (int i = 0; i < 7; ++i)
    put(wdt, out.mphd + 4 + i * 4, 1001u + static_cast<std::uint32_t>(i));
  out.main = add(wdt, "MAIN", 32768);
  for (unsigned i = 0; i < 4096; ++i)
  {
    put(wdt, out.main + i * 8, (i % 3 == 0) ? 0x2u : 0x0u);
    put(wdt, out.main + i * 8 + 4, 0x5000u + i);
  }
  if (with_maid)
    out.maid = add(wdt, "MAID", 131072);
  if (with_mai2)
    out.mai2 = add(wdt, "MAI2", 128);
  return wdt;
}

static void verifyWdtPreservation(std::string const& client, Bytes const& wdt,
                                  WdtLayout const& layout, bool with_mai2)
{
  std::vector<std::uint32_t> tiles(4096);
  for (unsigned i = 0; i < 4096; ++i)
    tiles[i] = (i % 3 == 0) ? 0x0u : 0x1u; // flip every HasADT bit

  auto const original_flags = read(wdt, layout.mphd);
  auto out = updateWdt(wdt, 0x2u | 0x4u, tiles);

  check_msg(out.size() == wdt.size(), client + " WDT: size unchanged");
  check_msg(read(out, layout.mphd) == ((original_flags & ~0xFu) | 0x6u),
            client + " WDT: MPHD keeps the high flag bits, low nibble updated");

  bool fdids_ok = true;
  for (int i = 0; i < 7; ++i)
    fdids_ok = fdids_ok && read(out, layout.mphd + 4 + i * 4) == 1001u + static_cast<std::uint32_t>(i);
  check_msg(fdids_ok, client + " WDT: all seven MPHD file ids preserved");

  bool maid_ok = layout.maid != 0;
  for (std::size_t i = 0; maid_ok && i < 131072; i += 997)
    maid_ok = out[layout.maid + i] == wdt[layout.maid + i];
  check_msg(maid_ok, client + " WDT: MAID preserved");

  bool mai2_ok = true;
  for (std::size_t i = 0; with_mai2 && i < 128; ++i)
    mai2_ok = mai2_ok && out[layout.mai2 + i] == wdt[layout.mai2 + i];
  check_msg(!with_mai2 || mai2_ok, client + " WDT: MAI2 preserved");

  bool main_ok = true;
  for (unsigned i = 0; i < 4096; ++i)
  {
    auto const original = read(wdt, layout.main + i * 8);
    auto const updated = read(out, layout.main + i * 8);
    main_ok = main_ok
      && (updated & 1u) == (tiles[i] & 1u)
      && (updated & ~1u) == (original & ~1u)
      && read(out, layout.main + i * 8 + 4) == 0x5000u + i;
  }
  check_msg(main_ok, client + " WDT: MAIN bit 0 updated, other bits and async ids preserved");
}

int main()
{
  std::cout << "per-client WDT/WDL preservation spec\n";

  // Shadowlands 9.2.7: legacy shading + big alpha + height texturing +
  // load _lod + wdt_has_maid, MAID present, no MAI2.
  {
    WdtLayout layout;
    auto const wdt = makeModernWdt(0x2u | 0x4u | 0x80u | 0x100u | 0x200u, true, false, layout);
    verifyWdtPreservation("Shadowlands", wdt, layout, false);
  }

  // Retail 12.x and Forever 1.60.x additionally carry MAI2; Forever's own
  // client files prove it (Kalimdor.wdt of build 1.60.1.70685).
  for (auto const* client : {"Retail", "Forever"})
  {
    WdtLayout layout;
    auto const wdt = makeModernWdt(0x2u | 0x4u | 0x80u | 0x100u | 0x200u, true, true, layout);
    verifyWdtPreservation(client, wdt, layout, true);
  }

  // Modern WDL: MAOF -> MARE(+MAHO) tiles, then the object LOD and sky-scene
  // chunks each client family carries. Heights update in place; nothing else
  // may move, and missing topology must be refused rather than invented.
  {
    Bytes wdl;
    put(wdl, add(wdl, "MVER", 4), 18);
    auto const maof = add(wdl, "MAOF", 16384);
    std::memset(wdl.data() + maof, 0, 16384);

    auto const mare0 = add(wdl, "MARE", 1090);
    put(wdl, maof + 0 * 4, static_cast<std::uint32_t>(mare0 - 8));
    auto const maho0 = add(wdl, "MAHO", 32);
    auto const mare1 = add(wdl, "MARE", 1090); // tile without MAHO
    put(wdl, maof + 1 * 4, static_cast<std::uint32_t>(mare1 - 8));

    auto const mldd = add(wdl, "MLDD", 36 * 4);
    add(wdl, "MLDX", 28 * 4);
    add(wdl, "MLMD", 40 * 4);
    add(wdl, "MLMX", 28 * 4);
    add(wdl, "MLDF", 40 * 4);   // Forever's Kalimdor.wdl carries MLDF
    add(wdl, "MSSN", 64);       // Shadowlands sky scenes
    add(wdl, "MSSC", 32);
    add(wdl, "MSSO", 32);
    add(wdl, "MSSF", 48);       // retail strip data

    auto const offsets = wdlOffsets(wdl);
    check_msg(offsets.size() == 2 && offsets.count(0) && offsets.count(1),
              "modern WDL: both MARE tiles are indexed");
    check_msg(offsets.at(0).holes.has_value() && !offsets.at(1).holes.has_value(),
              "modern WDL: tile 0 MAHO detected, tile 1 has none");

    std::int16_t outer[289];
    std::int16_t inner[256];
    for (int i = 0; i < 289; ++i) outer[i] = static_cast<std::int16_t>(100 + i);
    for (int i = 0; i < 256; ++i) inner[i] = static_cast<std::int16_t>(-100 - i);
    WdlHoles holes{};
    holes[3] = 0x8001;

    auto patched = wdl;
    updateWdlTile(patched, offsets, 0, outer, inner, holes);

    check_msg(patched.size() == wdl.size(), "modern WDL: size unchanged");
    check_msg(std::memcmp(patched.data() + mare0, outer, 578) == 0
              && std::memcmp(patched.data() + mare0 + 578, inner, 512) == 0,
              "modern WDL: tile 0 heights updated in place");
    check_msg(std::memcmp(patched.data() + *offsets.at(0).holes, holes.data(), 32) == 0,
              "modern WDL: tile 0 MAHO updated");

    bool untouched = true;
    for (std::size_t i = 0; i < wdl.size(); ++i)
    {
      bool const in_mare0 = (i >= mare0 && i < mare0 + 1090);
      bool const in_maho0 = (i >= maho0 && i < maho0 + 32);
      if (!in_mare0 && !in_maho0)
        untouched = untouched && patched[i] == wdl[i];
    }
    check_msg(untouched,
              "modern WDL: MAOF, tile 1, MLDD/MLDX/MLMD/MLMX/MLDF and MSSN/MSSC/MSSO/MSSF untouched");
    check_msg(std::memcmp(patched.data() + mldd, wdl.data() + mldd, 36 * 4) == 0,
              "modern WDL: object LOD records byte-identical");

    // A MAHO-less tile rejects hole data instead of inventing the chunk...
    bool threw = false;
    try { updateWdlTile(patched, offsets, 1, outer, inner, holes); }
    catch (std::runtime_error const&) { threw = true; }
    check_msg(threw, "modern WDL: holes for a MAHO-less tile are refused, not invented");

    // ...but hole-free height updates still work on it.
    WdlHoles no_holes{};
    updateWdlTile(patched, offsets, 1, outer, inner, no_holes);
    check_msg(std::memcmp(patched.data() + mare1, outer, 578) == 0,
              "modern WDL: MAHO-less tile heights still update");

    // A tile with no WDL entry at all: topology rebuild is unsupported.
    threw = false;
    try { updateWdlTile(patched, offsets, 2, outer, inner, no_holes); }
    catch (std::runtime_error const&) { threw = true; }
    check_msg(threw, "modern WDL: an absent tile is refused instead of rebuilding topology");
  }

  std::cout << (failures ? "FAILURES: " : "all passed: ") << checks
            << " checks, " << failures << " failed\n";
  return failures ? 1 : 0;
}
