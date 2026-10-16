// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/Db2DefinitionFallback.hpp>

#include <noggit/Log.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Noggit::Database
{
  namespace
  {
    using BlizzardDatabaseLib::BlizzardDatabase;
    using BlizzardDatabaseLib::BlizzardDatabaseTable;
    using BlizzardDatabaseLib::Stream::IMemStream;
    using BlizzardDatabaseLib::Structures::Build;
    using BlizzardDatabaseLib::Structures::VersionDefinition;

    struct BuildKey
    {
      bool valid = false;
      int expansion = 0;
      int major = 0;
      int minor = 0;
      long long build = 0;
    };

    BuildKey parseBuildKey(std::string const& text)
    {
      BuildKey key;

      std::vector<std::string> parts;
      std::string current;
      for (char character : text)
      {
        if (character == '.')
        {
          parts.push_back(current);
          current.clear();
        }
        else if (!std::isspace(static_cast<unsigned char>(character)))
        {
          current += character;
        }
      }
      parts.push_back(current);

      // BlizzardDatabaseLib::Structures::Build indexes four elements without
      // bounds checking, so never hand it anything else.
      if (parts.size() != 4)
        return key;

      for (auto const& part : parts)
        if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos)
          return key;

      key.expansion = std::atoi(parts[0].c_str());
      key.major = std::atoi(parts[1].c_str());
      key.minor = std::atoi(parts[2].c_str());
      key.build = std::atoll(parts[3].c_str());
      key.valid = true;

      return key;
    }

    std::uint64_t buildDistance(BuildKey const& candidate, BuildKey const& reference)
    {
      if (!candidate.valid || !reference.valid)
        return std::numeric_limits<std::uint64_t>::max();

      auto const difference = [](long long lhs, long long rhs)
      {
        return static_cast<std::uint64_t>(std::llabs(lhs - rhs));
      };

      std::uint64_t distance = 0;
      distance += difference(candidate.expansion, reference.expansion) * 1000000000ull;
      distance += difference(candidate.major, reference.major) * 10000000ull;
      distance += difference(candidate.minor, reference.minor) * 100000ull;
      distance += std::min<std::uint64_t>(difference(candidate.build, reference.build), 99999ull);

      return distance;
    }

    std::string trim(std::string text)
    {
      auto const is_space = [](unsigned char character)
      {
        return std::isspace(character) != 0;
      };

      while (!text.empty() && is_space(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
      while (!text.empty() && is_space(static_cast<unsigned char>(text.back())))
        text.pop_back();

      return text;
    }

    std::vector<std::string> splitOnComma(std::string const& text)
    {
      std::vector<std::string> values;
      std::string current;
      for (char character : text)
      {
        if (character == ',')
        {
          values.push_back(trim(current));
          current.clear();
        }
        else
        {
          current += character;
        }
      }
      values.push_back(trim(current));
      return values;
    }

    // The header is parsed by hand so that a definition can be picked before the
    // library needs one. These offsets are the file offsets of the fields used
    // below: four bytes of magic in front of the library's header structs. If the
    // library ever moves a field, the build breaks here instead of silently
    // reading the wrong numbers.
    namespace HeaderOffsets
    {
      constexpr std::size_t magic_size = 4;

      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDC5Header, RecordsCount) == 136);
      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDC5Header, FieldsCount) == 140);
      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDC5Header, RecordSize) == 144);
      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDC5Header, LayoutHash) == 156);

      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDC3Header, RecordsCount) == 4);
      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDC3Header, FieldsCount) == 8);
      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDC3Header, RecordSize) == 12);
      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDC3Header, LayoutHash) == 24);

      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDBCHeader, RecordsCount) == 4);
      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDBCHeader, FieldsCount) == 8);
      static_assert(magic_size + offsetof(BlizzardDatabaseLib::Structures::WDBCHeader, RecordSize) == 12);
    }

    template<typename T>
    bool readField(char const* data, std::size_t size, std::size_t offset, T& value)
    {
      if (offset + sizeof(T) > size)
        return false;
      std::memcpy(&value, data + offset, sizeof(T));
      return true;
    }

    std::string hexText(std::uint32_t value)
    {
      std::ostringstream stream;
      stream << std::hex << std::uppercase << value;
      return stream.str();
    }

    //! Keeps the databases used for substituted definitions alive: the tables they
    //! hand out stay referenced by the callers for the whole session.
    struct FallbackRegistry
    {
      // recursive: the failure helpers lock while a caller already holds it
      std::recursive_mutex mutex;
      std::unordered_map<std::string, std::shared_ptr<BlizzardDatabase>> databases;
      std::unordered_map<std::string, BlizzardDatabaseTable*> tables;
      std::unordered_map<std::string, VersionDefinition> definitions;
      std::unordered_set<std::string> failures;
      std::unordered_set<std::string> reported;
      std::size_t substituted_count = 0;
    };

    FallbackRegistry& registry()
    {
      static FallbackRegistry instance;
      return instance;
    }

    bool isDb2Build(std::string const& client_build, bool prefer_db2)
    {
      if (prefer_db2)
        return true;

      auto const key = parseBuildKey(client_build);
      return key.valid && (key.expansion > 7 || (key.expansion == 7 && key.major > 0)
                           || (key.expansion == 7 && key.major == 0 && key.minor > 3));
    }

    std::vector<char> readClientDatabaseFile(ClientDatabaseFileCallback const& callback,
                                             std::string const& table_name,
                                             bool db2_format)
    {
      std::vector<std::string> candidates;
      if (db2_format)
        candidates = {"DBFilesClient\\" + table_name + ".db2", "DBFilesClient\\" + table_name + ".dbc"};
      else
        candidates = {"DBFilesClient\\" + table_name + ".dbc", "DBFilesClient\\" + table_name + ".db2"};

      for (auto const& filename : candidates)
      {
        try
        {
          auto stream = callback(filename);
          if (!stream)
            continue;

          std::vector<char> bytes((std::istreambuf_iterator<char>(*stream)),
                                  std::istreambuf_iterator<char>());
          if (bytes.size() >= 8)
            return bytes;
        }
        catch (std::exception const&)
        {
          // try the other extension
        }
      }

      return {};
    }

    struct SubstitutionAttempt
    {
      BlizzardDatabaseTable* table = nullptr;
      std::string build;
      std::string reason;
      std::string error;
      int definition_columns = 0;
      bool layout_matches = false;
      bool field_count_matches = false;
      //! Only a definition that provably describes the file may be used: a wrong
      //! column count makes the readers walk off the end of a record.
      bool accepted = false;
    };

    bool blockDeclaresLayout(std::vector<DefinitionBlock> const& blocks,
                             std::string const& build,
                             std::uint32_t layout_hash)
    {
      if (layout_hash == 0)
        return false;

      for (auto const& block : blocks)
      {
        if (std::find(block.builds.begin(), block.builds.end(), build) == block.builds.end())
          continue;

        if (std::find(block.layout_hashes.begin(), block.layout_hashes.end(), layout_hash)
            != block.layout_hashes.end())
          return true;
      }

      return false;
    }

    //! DatabaseDefinition::Read() dereferences the first line of the file without
    //! checking that there is one, so a missing or empty .dbd takes the whole
    //! application down. Never let the library open one of those.
    bool definitionFileIsUsable(std::filesystem::path const& dbd_file)
    {
      std::error_code error;
      if (!std::filesystem::exists(dbd_file, error) || error)
        return false;

      auto const size = std::filesystem::file_size(dbd_file, error);
      return !error && size > 0;
    }

    //! Records the failure so it is neither retried nor logged again, then throws.
    [[noreturn]] void failLoad(std::string const& cache_key,
                               std::string const& exception_message,
                               std::string const& log_message)
    {
      {
        std::lock_guard<std::recursive_mutex> lock(registry().mutex);
        registry().failures.insert(cache_key);

        if (!log_message.empty() && registry().reported.insert(cache_key).second)
          LogError << log_message << std::endl;
      }

      throw std::runtime_error(exception_message);
    }

    BlizzardDatabase& fallbackDatabase(std::string const& definitions_directory,
                                       std::string const& build,
                                       bool prefer_db2)
    {
      auto const key = definitions_directory + "|" + build + "|" + (prefer_db2 ? "db2" : "dbc");

      auto& shared = registry().databases;
      auto const existing = shared.find(key);
      if (existing != shared.end())
        return *existing->second;

      auto database = std::make_shared<BlizzardDatabase>(definitions_directory, Build(build), prefer_db2);
      shared.emplace(key, database);
      return *database;
    }
  }

  std::vector<DefinitionBlock> parseDefinitionBlocks(std::filesystem::path const& dbd_file)
  {
    std::vector<DefinitionBlock> blocks;

    std::ifstream stream(dbd_file);
    if (!stream.is_open())
      return blocks;

    std::string line;
    DefinitionBlock current;
    bool reading_columns = true;

    while (std::getline(stream, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      if (line.empty())
      {
        if (reading_columns)
        {
          // blank line ends the COLUMNS section, the version blocks follow
          reading_columns = false;
          current = DefinitionBlock{};
          continue;
        }

        if (!current.builds.empty() || !current.layout_hashes.empty() || current.field_count > 0)
          blocks.push_back(current);
        current = DefinitionBlock{};
        continue;
      }

      auto const starts_with = [&line](char const* keyword)
      {
        return line.compare(0, std::strlen(keyword), keyword) == 0;
      };

      if (starts_with("COLUMNS"))
      {
        reading_columns = true;
        continue;
      }

      if (reading_columns)
        continue;

      if (starts_with("LAYOUT"))
      {
        for (auto const& hash : splitOnComma(line.substr(std::strlen("LAYOUT"))))
        {
          if (hash.empty())
            continue;
          try
          {
            current.layout_hashes.push_back(static_cast<std::uint32_t>(std::stoul(hash, nullptr, 16)));
          }
          catch (std::exception const&)
          {
            // malformed hash, ignore it
          }
        }
        continue;
      }

      if (starts_with("BUILD"))
      {
        for (auto const& value : splitOnComma(line.substr(std::strlen("BUILD"))))
        {
          if (value.empty())
            continue;

          // a build range ("1.2.3.4-1.2.3.9") describes every build in between,
          // both ends are valid stand-ins for it
          auto const dash = value.find('-');
          if (dash != std::string::npos)
          {
            auto const from = parseBuildKey(value.substr(0, dash));
            auto const to = parseBuildKey(value.substr(dash + 1));
            if (from.valid)
              current.builds.push_back(trim(value.substr(0, dash)));
            if (to.valid)
              current.builds.push_back(trim(value.substr(dash + 1)));
          }
          else if (parseBuildKey(value).valid)
          {
            current.builds.push_back(value);
          }
        }
        continue;
      }

      if (starts_with("COMMENT"))
        continue;

      ++current.field_count;
    }

    // the last block is not always followed by a blank line; the trailing blank
    // line most files end with must not turn into an empty block either
    if (!reading_columns
        && (!current.builds.empty() || !current.layout_hashes.empty() || current.field_count > 0))
      blocks.push_back(current);

    return blocks;
  }

  ClientDatabaseFileHeader readClientDatabaseHeader(char const* data, std::size_t size)
  {
    ClientDatabaseFileHeader header;

    if (!data || size < 8)
      return header;

    header.magic.assign(data, 4);

    std::uint32_t layout_hash = 0;
    std::int32_t records_count = 0;
    std::int32_t fields_count = 0;
    std::int32_t record_size = 0;
    bool parsed = false;

    // The offsets below mirror BlizzardDatabaseLib::Structures::WDC5Header,
    // WDC3Header and WDBCHeader: the magic is not part of the structs, so
    // everything is shifted by four bytes.
    auto const parse_at = [&](std::size_t base, bool with_layout_hash)
    {
      return readField(data, size, base + 0, records_count)
             && readField(data, size, base + 4, fields_count)
             && readField(data, size, base + 8, record_size)
             && (!with_layout_hash || readField(data, size, base + 20, layout_hash));
    };

    if (header.magic == "WDC5" || header.magic == "WDC4")
    {
      // schema version (4) + schema string (128) come first
      std::size_t const schema_base = HeaderOffsets::magic_size + sizeof(std::uint32_t) + 128;
      if (parse_at(schema_base, true))
        parsed = true;

      // WDC4 files are read as WDC5 by the library, but not every build ships the
      // schema string: fall back to the compact layout when the numbers are silly.
      auto const plausible = fields_count > 0 && fields_count < 4096 && records_count >= 0;
      if (!plausible && parse_at(HeaderOffsets::magic_size, true))
        parsed = true;
    }
    else if (header.magic == "WDC3" || header.magic == "WDC2" || header.magic == "WDC1")
    {
      parsed = parse_at(HeaderOffsets::magic_size, true);
    }
    else if (header.magic == "WDBC")
    {
      parsed = parse_at(HeaderOffsets::magic_size, false);
    }

    if (!parsed)
      return header;

    header.layout_hash = layout_hash;
    header.records_count = records_count;
    header.fields_count = fields_count;
    header.record_size = record_size;
    header.valid = fields_count > 0 && fields_count < 4096 && records_count >= 0 && record_size >= 0;

    return header;
  }

  std::vector<std::string> orderCandidateBuilds(std::vector<DefinitionBlock> const& blocks,
                                                ClientDatabaseFileHeader const& header,
                                                std::string const& client_build,
                                                std::size_t max_candidates)
  {
    // Without a readable header there is nothing to match a definition against.
    // Callers report the unreadable file instead of guessing.
    if (!header.valid || max_candidates == 0)
      return {};

    auto const reference = parseBuildKey(client_build);

    struct Candidate
    {
      std::string build;
      int tier = 0;
      std::uint64_t distance = 0;
    };

    // Every build of a block shares one definition, so a block is tried once with
    // its closest build as the representative. Collecting builds instead would
    // spend all the attempts on a single definition.
    std::map<std::size_t, Candidate> best_per_block;

    auto const consider = [&best_per_block, &reference](std::size_t index,
                                                        DefinitionBlock const& block,
                                                        int tier)
    {
      for (auto const& build : block.builds)
      {
        auto const key = parseBuildKey(build);
        if (!key.valid)
          continue;

        auto const distance = buildDistance(key, reference);
        auto const existing = best_per_block.find(index);
        if (existing != best_per_block.end())
        {
          // a better tier always wins, otherwise the closest build
          if (existing->second.tier < tier
              || (existing->second.tier == tier && existing->second.distance <= distance))
            continue;
        }

        best_per_block[index] = Candidate{build, tier, distance};
      }
    };

    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
      auto const& block = blocks[index];

      // tier 0 - the block that declares this exact layout
      if (header.valid && header.layout_hash != 0
          && std::find(block.layout_hashes.begin(), block.layout_hashes.end(), header.layout_hash)
               != block.layout_hashes.end())
      {
        consider(index, block, 0);
        continue;
      }

      // tier 1 - a block describing the same number of columns
      if (header.valid && header.fields_count > 0 && block.field_count == header.fields_count)
      {
        consider(index, block, 1);
        continue;
      }

      // tier 2 - a block from the same expansion, closest build first
      if (reference.valid)
        for (auto const& build : block.builds)
        {
          auto const key = parseBuildKey(build);
          if (key.valid && key.expansion == reference.expansion)
          {
            consider(index, block, 2);
            break;
          }
        }
    }

    std::vector<Candidate> candidates;
    candidates.reserve(best_per_block.size());
    for (auto& [index, candidate] : best_per_block)
      candidates.push_back(candidate);

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](Candidate const& lhs, Candidate const& rhs)
                     {
                       if (lhs.tier != rhs.tier)
                         return lhs.tier < rhs.tier;
                       return lhs.distance < rhs.distance;
                     });

    std::vector<std::string> ordered;
    for (auto const& candidate : candidates)
    {
      ordered.push_back(candidate.build);
      if (ordered.size() >= max_candidates)
        break;
    }

    return ordered;
  }

  BlizzardDatabaseTable& loadTableWithDefinitionFallback(BlizzardDatabase& primary,
                                                         std::string const& table_name,
                                                         ClientDatabaseContext const& context,
                                                         ClientDatabaseFileCallback const& file_callback,
                                                         char const* log_context)
  {
    auto const cache_key = context.definitions_directory + "|" + context.client_build + "|"
                           + (context.prefer_db2 ? "db2" : "dbc") + "|" + table_name;
    auto const dbd_file = std::filesystem::path(context.definitions_directory) / (table_name + ".dbd");

    {
      std::lock_guard<std::recursive_mutex> lock(registry().mutex);

      auto const cached_table = registry().tables.find(cache_key);
      if (cached_table != registry().tables.end())
        return *cached_table->second;

      if (registry().failures.count(cache_key) != 0)
        throw std::runtime_error("Database definition build/layout not found for " + table_name);
    }

    if (!definitionFileIsUsable(dbd_file))
      failLoad(cache_key,
               "Database definition file not found for " + table_name,
               std::string(log_context) + " " + table_name + ": no database definition at "
                 + dbd_file.generic_string() + ", check the definitions directory ("
                 + (context.definitions_directory.empty() ? "<empty>" : context.definitions_directory)
                 + ")");

    std::string primary_error;
    try
    {
      auto& table = primary.LoadTable(table_name, file_callback);

      // Remember the loaded table: the database library prints a line and walks a
      // map for every repeated request, and some callers ask once per record.
      {
        std::lock_guard<std::recursive_mutex> lock(registry().mutex);
        registry().tables.emplace(cache_key, &table);
      }

      return table;
    }
    catch (std::exception const& exception)
    {
      primary_error = exception.what();
    }
    catch (...)
    {
      primary_error = "unknown error";
    }

    std::lock_guard<std::recursive_mutex> lock(registry().mutex);

    auto& shared = registry();

    // another thread may have substituted a definition while this one was reading
    auto const cached_table = shared.tables.find(cache_key);
    if (cached_table != shared.tables.end())
      return *cached_table->second;

    auto const blocks = parseDefinitionBlocks(dbd_file);
    auto const file_bytes = readClientDatabaseFile(file_callback, table_name,
                                                   isDb2Build(context.client_build, context.prefer_db2));
    auto const header = readClientDatabaseHeader(file_bytes.empty() ? nullptr : file_bytes.data(),
                                                 file_bytes.size());

    if (file_bytes.empty() || !header.valid)
      failLoad(cache_key,
               "Could not read client database file for " + table_name,
               std::string(log_context) + " " + table_name
                 + ": could not read the client database file header (" + primary_error + ")");

    // The file itself is already in memory: retrying with another definition must
    // not hit CASC again.
    auto const cached_callback = [&file_bytes](std::string const&)
    {
      return std::make_shared<IMemStream>(file_bytes.data(), file_bytes.size());
    };

    std::size_t constexpr max_attempts = 4;
    auto const candidates = orderCandidateBuilds(blocks, header, context.client_build, max_attempts);

    std::vector<SubstitutionAttempt> attempts;
    for (auto const& candidate_build : candidates)
    {
      SubstitutionAttempt attempt;
      attempt.build = candidate_build;

      attempt.layout_matches = blockDeclaresLayout(blocks, candidate_build, header.layout_hash);

      try
      {
        auto& database = fallbackDatabase(context.definitions_directory, candidate_build,
                                          context.prefer_db2);
        auto& table = database.LoadTable(table_name, cached_callback);

        attempt.table = &table;
        attempt.definition_columns =
          static_cast<int>(table.GetRecordDefinition().ColumnDefinitions.size());
        attempt.field_count_matches = attempt.definition_columns == header.fields_count;
        attempt.accepted = attempt.layout_matches || attempt.field_count_matches;
        attempt.reason = attempt.layout_matches
                           ? "layout " + hexText(header.layout_hash)
                           : std::to_string(header.fields_count) + " matching columns";
      }
      catch (std::exception const& exception)
      {
        attempt.error = exception.what();
        LogDebug << log_context << " " << table_name << ": definition for build " << candidate_build
                 << " did not load (" << exception.what() << ")" << std::endl;
      }

      attempts.push_back(attempt);

      if (attempt.accepted)
        break;
    }

    SubstitutionAttempt const* winner = nullptr;
    for (auto const& attempt : attempts)
      if (attempt.accepted && attempt.table)
      {
        winner = &attempt;
        break;
      }

    if (!winner)
    {
      std::ostringstream message;
      message << log_context << " " << table_name << ".db2: client build " << context.client_build
              << " is newer than the definitions in " << context.definitions_directory
              << ". The file has layout " << hexText(header.layout_hash) << ", " << header.fields_count
              << " columns and " << header.records_count << " records, but none of the definition"
              << " blocks describes it:";

      for (auto const& attempt : attempts)
      {
        message << "\n    " << attempt.build << ": ";
        if (attempt.table)
          message << attempt.definition_columns << " columns (does not match the file)";
        else
          message << "did not load (" << attempt.error << ")";
      }

      message << "\n  Update the DBD definitions to read this table.";

      failLoad(cache_key, "Database definition build/layout not found for " + table_name,
               message.str());
    }

    shared.tables[cache_key] = winner->table;

    if (shared.definitions.find(table_name) == shared.definitions.end())
    {
      // BlizzardDatabase::TableDefinition() hands out a reference to a local in
      // this version of the library, so build the definition from the reader
      // itself: it owns the pruned row definition that was actually used.
      VersionDefinition definition;
      definition.tableName = table_name;
      definition.useGlobalStringOffsets = context.prefer_db2;
      definition.RowDefinition = winner->table->GetRecordDefinition();

      auto const& columns = definition.RowDefinition.ColumnDefinitions;
      for (std::size_t index = 0; index < columns.size(); ++index)
      {
        if (!columns[index].isID)
          continue;

        definition.hasId = true;
        definition.idColumnIndex = static_cast<int>(index);
        break;
      }

      shared.definitions.emplace(table_name, definition);
    }

    ++shared.substituted_count;

    if (shared.reported.insert(cache_key).second)
    {
      Log << "[DB2Definitions] " << table_name << ".db2: client build " << context.client_build
          << " is not described by " << dbd_file.generic_string() << ", using the " << winner->build
          << " definition instead (" << winner->reason << ", " << winner->table->RecordCount()
          << " records)" << std::endl;
    }

    return *winner->table;
  }

  VersionDefinition* substitutedDefinition(std::string const& table_name)
  {
    std::lock_guard<std::recursive_mutex> lock(registry().mutex);
    auto const definition = registry().definitions.find(table_name);
    return definition == registry().definitions.end() ? nullptr : &definition->second;
  }

  std::size_t substitutedDefinitionCount()
  {
    std::lock_guard<std::recursive_mutex> lock(registry().mutex);
    return registry().substituted_count;
  }
}
