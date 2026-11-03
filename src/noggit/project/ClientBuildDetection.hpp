// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

// Client build detection, split out of ApplicationProject.cpp so it can be
// unit-tested without Qt. Resolves the build string of the client a project
// points at: .build.info rows (preferring the product line the project needs
// over the Active! retail row of multi-product install roots), falling back
// to Data/config build-name files.
#include <noggit/Log.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace Noggit::Project
{
  inline bool fileExistsWithContent(std::filesystem::path const& path)
  {
    std::error_code error;
    return std::filesystem::exists(path, error) && std::filesystem::is_regular_file(path, error)
      && std::filesystem::file_size(path, error) > 0;
  }

  inline std::vector<std::string> splitBuildInfoLine(std::string const& line)
  {
    std::vector<std::string> values;
    std::string value;
    std::stringstream stream(line);

    while (std::getline(stream, value, '|'))
      values.push_back(value);

    return values;
  }

  inline std::string detectClientBuildVersion(std::filesystem::path const& raw_client_path,
                                       std::string const& preferred_product)
  {
    std::vector<std::filesystem::path> candidates;
    auto client_path = raw_client_path;

    candidates.push_back(client_path / ".build.info");

    if (client_path.filename() == "_retail_")
      candidates.push_back(client_path.parent_path() / ".build.info");
    else
      candidates.push_back(client_path / "_retail_" / ".build.info");

    for (auto const& build_info_path : candidates)
    {
      if (!fileExistsWithContent(build_info_path))
        continue;

      std::ifstream stream(build_info_path);
      std::string header_line;
      if (!std::getline(stream, header_line))
        continue;

      auto const headers = splitBuildInfoLine(header_line);
      int version_index = -1;
      int active_index = -1;
      int product_index = -1;

      for (std::size_t i = 0; i < headers.size(); ++i)
      {
        if (headers[i].rfind("Version!", 0) == 0)
          version_index = static_cast<int>(i);
        else if (headers[i].rfind("Active!", 0) == 0)
          active_index = static_cast<int>(i);
        else if (headers[i].rfind("Product!", 0) == 0)
          product_index = static_cast<int>(i);
      }

      if (version_index < 0)
        continue;

      std::string first_version;
      std::string active_version;
      std::string line;
      while (std::getline(stream, line))
      {
        auto const values = splitBuildInfoLine(line);
        if (version_index >= static_cast<int>(values.size()) || values[version_index].empty())
          continue;

        // The product the project actually needs wins. Multi-product
        // .build.info files (Battle.net install roots) mark retail as the
        // active product, so preferring Active! made every client selected
        // from the same install detect - and load - the retail build.
        if (product_index >= 0 && product_index < static_cast<int>(values.size())
            && !preferred_product.empty() && values[product_index] == preferred_product)
        {
          Log << "[Client] Detected build " << values[version_index] << " for product '"
              << preferred_product << "' from " << build_info_path << std::endl;
          return values[version_index];
        }

        if (first_version.empty())
          first_version = values[version_index];

        if (active_version.empty()
            && (active_index < 0
                || (active_index < static_cast<int>(values.size()) && values[active_index] == "1")))
          active_version = values[version_index];
      }

      if (!active_version.empty())
      {
        Log << "[Client] Detected active-product build " << active_version << " from "
            << build_info_path << (preferred_product.empty()
                                     ? std::string()
                                     : " (product '" + preferred_product + "' not listed)")
            << std::endl;
        return active_version;
      }

      if (!first_version.empty())
      {
        Log << "[Client] Detected first-listed build " << first_version << " from "
            << build_info_path << std::endl;
        return first_version;
      }
    }

    if (client_path.filename() == "_retail_")
      client_path = client_path.parent_path();
    else if (!std::filesystem::exists(client_path / "Data" / "config")
             && std::filesystem::exists(client_path.parent_path() / "Data" / "config"))
      client_path = client_path.parent_path();

    auto const config_path = client_path / "Data" / "config";
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
      config_path, std::filesystem::directory_options::skip_permission_denied, error);
    std::filesystem::recursive_directory_iterator end;
    // Accept every Blizzard build-name family (WOW-, WOWT- PTR, WOWB- beta,
    // WOWCLASSIC-... and the Forever/classic-era names); the old WOW-only
    // pattern silently failed detection for those clients.
    static std::regex const build_name_pattern(
      R"(WOW[A-Z_]*-([0-9]+)patch([0-9]+\.[0-9]+\.[0-9]+))", std::regex::icase);

    for (; !error && iterator != end; iterator.increment(error))
    {
      if (!iterator->is_regular_file(error))
        continue;

      std::ifstream stream(iterator->path());
      std::string line;
      if (!std::getline(stream, line))
        continue;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line != "# Build Configuration")
        continue;

      while (std::getline(stream, line))
      {
        if (line.rfind("build-name", 0) != 0)
          continue;

        std::smatch match;
        if (std::regex_search(line, match, build_name_pattern))
        {
          // Expansion first: "WOW-9patch2.7.45745" is client build 9.2.7.45745.
          // The old code returned the components reversed ("2.7.45745.9"), so
          // build parsing saw a minor version as the major and definition
          // matching could never hit the real build ranges.
          auto const version = match[1].str() + "." + match[2].str();
          Log << "[Client] Detected build " << version << " from " << iterator->path() << std::endl;
          return version;
        }
      }
    }

    return {};
  }
}
