// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// Regression test for non-retail modern object companion pairing
// (src/noggit/formats/adt/RetailObjectTables.hpp::pairPlacementBytes).
//
// On Shadowlands and Forever, placement edits used to rewrite OBJ0 MDDF/MODF
// (and OBJ1 MLDD/MLMD) while leaving the one-byte-per-placement companion
// tables (OBJ0 MLMB, OBJ1 MLDB) at their original size and order. The client
// reads those tables positionally, so any add/remove/reorder produced a
// count-mismatched or mispaired table - the out-of-bounds crash class the
// retail path already guards against via remap()/validate().
#include <noggit/formats/adt/RetailObjectTables.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
  using Noggit::Formats::ADT::RetailObjectTables::Bytes;
  using Noggit::Formats::ADT::RetailObjectTables::chunks;
  using Noggit::Formats::ADT::RetailObjectTables::pairPlacementBytes;
  using Noggit::Formats::ADT::RetailObjectTables::validate;

  // Mirrors RetailObjectTransfer.hpp defaults (that header pulls in glm).
  constexpr std::uint8_t defaultModelByte = 64, defaultWmoByte = 128;

  int checks = 0;
  int failures = 0;

  void check(bool ok, std::string const& what)
  {
    ++checks;
    if (!ok)
    {
      ++failures;
      std::cerr << "FAIL: " << what << '\n';
    }
  }

  void put32(Bytes& out, std::uint32_t value)
  {
    out.insert(out.end(), reinterpret_cast<std::uint8_t const*>(&value),
               reinterpret_cast<std::uint8_t const*>(&value) + 4);
  }

  // tag is given in on-disk byte order ("FDOM" is MODF on disk).
  Bytes chunk(char const* tag, Bytes const& payload)
  {
    Bytes out;
    out.insert(out.end(), tag, tag + 4);
    put32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
  }

  Bytes wmoEntry(std::uint32_t fdid, std::uint32_t uid, std::uint32_t options = 0)
  {
    Bytes entry(64, 0);
    std::memcpy(entry.data() + 0, &fdid, 4);
    std::memcpy(entry.data() + 4, &uid, 4);
    std::memcpy(entry.data() + 56, &options, 4);
    return entry;
  }

  Bytes m2Entry(std::uint32_t fdid, std::uint32_t uid)
  {
    Bytes entry(36, 0);
    std::memcpy(entry.data() + 0, &fdid, 4);
    std::memcpy(entry.data() + 4, &uid, 4);
    return entry;
  }

  Bytes cat(Bytes a, Bytes const& b)
  {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  }

  bool throws(std::function<void()> const& fn)
  {
    try { fn(); } catch (std::exception const&) { return true; }
    return false;
  }
}

int main()
{
  Bytes const mver = chunk("REVM", Bytes{18, 0, 0, 0});

  // 1: OBJ0 MLMB follows removes/adds/reorders; new WMOs get the default byte.
  {
    auto original = cat(mver, chunk("FDOM", cat(wmoEntry(100, 1), wmoEntry(200, 2))));
    original = cat(original, chunk("BMLM", Bytes{7, 9}));
    // What the codec produced before the fix: rewritten MODF, stale MLMB.
    auto stale = cat(mver, chunk("FDOM", cat(wmoEntry(200, 2), wmoEntry(300, 3))));
    stale = cat(stale, chunk("BMLM", Bytes{7, 9}));

    auto result = pairPlacementBytes(original, stale, "FDOM", 64, "BMLM",
                                     defaultWmoByte, "MLMB");
    auto table = chunks(result);
    check(table.count("BMLM") && table.at("BMLM").size == 2, "paired MLMB has one byte per WMO");
    check(result[table.at("BMLM").offset] == 9, "kept WMO carries its original MLMB byte");
    check(result[table.at("BMLM").offset + 1] == defaultWmoByte, "new WMO gets the default MLMB byte");
    check(!throws([&] { validate(result, false); }), "paired OBJ0 passes structural validation");
  }

  // 2: the stale layout the fix replaces is exactly what validation rejects.
  {
    auto mismatched = cat(mver, chunk("FDOM", cat(wmoEntry(100, 1), wmoEntry(200, 2))));
    mismatched = cat(mismatched, chunk("BMLM", Bytes{7, 9, 11}));
    check(throws([&] { validate(mismatched, false); }),
          "count-mismatched MLMB is rejected before it can reach the client");
  }

  // 3: reordering alone repairs the per-WMO pairing.
  {
    auto original = cat(mver, chunk("FDOM", cat(wmoEntry(100, 1), wmoEntry(200, 2))));
    original = cat(original, chunk("BMLM", Bytes{7, 9}));
    auto reordered = cat(mver, chunk("FDOM", cat(wmoEntry(200, 2), wmoEntry(100, 1))));
    reordered = cat(reordered, chunk("BMLM", Bytes{7, 9}));
    auto result = pairPlacementBytes(original, reordered, "FDOM", 64, "BMLM",
                                     defaultWmoByte, "MLMB");
    auto table = chunks(result);
    check(result[table.at("BMLM").offset] == 9 && result[table.at("BMLM").offset + 1] == 7,
          "reordered WMOs keep their own MLMB bytes");
  }

  // 4: OBJ1 MLDB follows the rebuilt MLDD list; new M2s get the default byte.
  {
    auto original = cat(mver, chunk("DDLM", cat(m2Entry(110, 11), m2Entry(120, 12))));
    original = cat(original, chunk("BDLM", Bytes{5, 6}));
    auto stale = cat(mver, chunk("DDLM", cat(m2Entry(120, 12), m2Entry(130, 13))));
    stale = cat(stale, chunk("BDLM", Bytes{5, 6}));
    auto result = pairPlacementBytes(original, stale, "DDLM", 36, "BDLM",
                                     defaultModelByte, "MLDB");
    auto table = chunks(result);
    check(table.count("BDLM") && table.at("BDLM").size == 2, "paired MLDB has one byte per doodad");
    check(result[table.at("BDLM").offset] == 6, "kept doodad carries its original MLDB byte");
    check(result[table.at("BDLM").offset + 1] == defaultModelByte, "new doodad gets the default MLDB byte");
    check(!throws([&] { validate(result, true); }), "paired OBJ1 passes structural validation");
  }

  // 5: an original without the companion table is passed through untouched.
  {
    auto original = cat(mver, chunk("FDOM", wmoEntry(100, 1)));
    auto updated = cat(mver, chunk("FDOM", cat(wmoEntry(100, 1), wmoEntry(300, 3))));
    auto result = pairPlacementBytes(original, updated, "FDOM", 64, "BMLM",
                                     defaultWmoByte, "MLMB");
    check(result == updated, "no MLMB in the original means nothing to pair");
  }

  // 6: ambiguous original identities are rejected instead of guessed.
  {
    auto original = cat(mver, chunk("FDOM", cat(wmoEntry(100, 1), wmoEntry(100, 1))));
    original = cat(original, chunk("BMLM", Bytes{7, 9}));
    auto updated = cat(mver, chunk("FDOM", wmoEntry(100, 1)));
    updated = cat(updated, chunk("BMLM", Bytes{7}));
    check(throws([&] {
      pairPlacementBytes(original, updated, "FDOM", 64, "BMLM", defaultWmoByte, "MLMB");
    }), "duplicate placement identities cannot be paired");
  }

  // 7: unrelated chunks survive the pairing rewrite byte-for-byte.
  {
    Bytes const mfbo = chunk("OBFM", Bytes(48, 3));
    auto original = cat(cat(mver, chunk("FDOM", wmoEntry(100, 1))), chunk("BMLM", Bytes{4}));
    original = cat(original, mfbo);
    auto updated = cat(cat(mver, chunk("FDOM", cat(wmoEntry(100, 1), wmoEntry(900, 9)))),
                       chunk("BMLM", Bytes{4}));
    updated = cat(updated, mfbo);
    auto result = pairPlacementBytes(original, updated, "FDOM", 64, "BMLM",
                                     defaultWmoByte, "MLMB");
    auto table = chunks(result);
    check(table.size() == 4, "pairing keeps every other chunk");
    check(std::memcmp(result.data() + table.at("OBFM").offset, mfbo.data() + 8, 48) == 0,
          "unrelated chunk payload is untouched");
    check(table.at("BMLM").size == 2, "paired table resized with the placements");
  }

  std::cout << (failures ? "FAILED " : "PASSED ") << (checks - failures) << '/' << checks
            << " modern object companion checks\n";
  return failures ? 1 : 0;
}
