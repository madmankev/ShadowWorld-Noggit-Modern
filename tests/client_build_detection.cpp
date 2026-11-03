// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// Regression test for client build detection (src/noggit/project/ClientBuildDetection.hpp).
//
// The project-selected client must decide which data gets loaded. Detection
// used to prefer the Active! row of a multi-product .build.info (always the
// retail product on a Battle.net install root), so every client selected from
// the same install detected as the retail build - and the load-time override
// then flipped the whole session to the Retail CASC profile. The Data/config
// fallback additionally rejected non-"WOW-" build-name families (WOWB-, WOWT-,
// WOWCLASSIC-...) and returned the version components in the wrong order
// ("2.7.45745.9" instead of "9.2.7.45745").
#include <noggit/project/ClientBuildDetection.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <syncstream>

// Log.cpp is Qt-tainted; provide the stream stubs the header expects.
std::osyncstream _LogError(const char*, int) { return std::osyncstream(std::cerr); }
std::osyncstream _LogDebug(const char*, int) { return std::osyncstream(std::cout); }
std::osyncstream _Log(const char*, int) { return std::osyncstream(std::cout); }

namespace
{
  int failures = 0;
  int checks = 0;

  void check(bool ok, std::string const& what)
  {
    ++checks;
    if (!ok)
    {
      ++failures;
      std::cerr << "FAIL: " << what << '\n';
    }
  }

  void check_eq(std::string const& actual, std::string const& expected, std::string const& what)
  {
    ++checks;
    if (actual != expected)
    {
      ++failures;
      std::cerr << "FAIL: " << what << " - expected '" << expected << "', got '" << actual << "'\n";
    }
  }

  void write_file(std::filesystem::path const& path, std::string const& content)
  {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
  }
}

int main()
{
  using Noggit::Project::detectClientBuildVersion;

  auto const root = std::filesystem::temp_directory_path() / "noggit_client_detection_test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  // A: multi-product install-root .build.info. Retail is the ACTIVE product;
  //    the product the project needs must win regardless of row order.
  write_file(root / "multi/.build.info",
    "Branch!STRING:0|Build Config!HEX:16|CDN Configs!HEX:16|CDN Path!STRING:0"
    "|Product!STRING:0|Version!STRING:0|Active!DEC:8\n"
    "wow|aa|bb|/|wow|12.1.5.70688|1\n"
    "wow_classic_beta|cc|dd|/|wow_classic_beta|1.60.1.70685|0\n"
    "wow_classic_era|ee|ff|/|wow_classic_era|1.15.9.62461|0\n");
  check_eq(detectClientBuildVersion(root / "multi", "wow"), "12.1.5.70688",
           "retail product row wins for 'wow'");
  check_eq(detectClientBuildVersion(root / "multi", "wow_classic_beta"), "1.60.1.70685",
           "Forever product row wins over the active retail row");
  check_eq(detectClientBuildVersion(root / "multi", ""), "12.1.5.70688",
           "no product preference falls back to the active row");

  // B: single-product standalone install without a Product column (epsilon-style).
  write_file(root / "epsilon/.build.info",
    "Branch!STRING:0|Version!STRING:0|Active!DEC:8\n"
    "wow|9.2.7.45745|1\n");
  check_eq(detectClientBuildVersion(root / "epsilon", "wow"), "9.2.7.45745",
           "single-product build info still detects");
  check_eq(detectClientBuildVersion(root / "epsilon", "wow_classic_beta"), "9.2.7.45745",
           "missing preferred product falls back to the active row");

  // C: Data/config fallback with a WOWB- (Forever/classic-beta family) build-name.
  write_file(root / "forever/Data/config/1a/2b/1a2b3c4d5e6f708192a3b4c5d6e7f809",
    "# Build Configuration\n"
    "build-name = WOWB-1patch60.1.70685\n");
  check_eq(detectClientBuildVersion(root / "forever", "wow_classic_beta"), "1.60.1.70685",
           "WOWB- config build-name detects with expansion first");

  // D: Data/config fallback ordering for the classic WOW- family.
  write_file(root / "sl/Data/config/aa/bb/ccbbaa99887766554433221100ffeedd",
    "# Build Configuration\n"
    "build-name = WOW-9patch2.7.45745\n");
  check_eq(detectClientBuildVersion(root / "sl", "wow"), "9.2.7.45745",
           "WOW- config build-name returns expansion.major.minor.build");

  // E: PTR-family build-name.
  write_file(root / "ptr/Data/config/cc/dd/eeddccbbaa99887766554433221100ff",
    "# Build Configuration\n"
    "build-name = WOWT-12patch1.5.71000\n");
  check_eq(detectClientBuildVersion(root / "ptr", "wow"), "12.1.5.71000",
           "WOWT- config build-name detects");

  // F: _retail_ flavor folders resolve through the install-root .build.info.
  std::filesystem::create_directories(root / "install/_retail_");
  write_file(root / "install/.build.info",
    "Product!STRING:0|Version!STRING:0|Active!DEC:8\n"
    "wow|12.1.5.70688|1\n");
  check_eq(detectClientBuildVersion(root / "install/_retail_", "wow"), "12.1.5.70688",
           "_retail_ redirects to the parent .build.info");

  // G: nothing detectable -> empty (loadProject applies its own fallback).
  std::filesystem::create_directories(root / "empty");
  check(detectClientBuildVersion(root / "empty", "wow").empty(),
        "empty client directory detects nothing");

  std::filesystem::remove_all(root, ec);

  std::cout << (failures ? "FAILED " : "PASSED ") << (checks - failures) << '/' << checks
            << " client build detection checks\n";
  return failures ? 1 : 0;
}
