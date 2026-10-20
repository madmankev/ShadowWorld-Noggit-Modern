// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DB2_DEFINITION_FALLBACK_HPP
#define NOGGIT_DB2_DEFINITION_FALLBACK_HPP

#include <blizzard-database-library/include/BlizzardDatabase.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Noggit::Database
{
  //! Where the DBD definitions live and which build they are supposed to describe.
  struct ClientDatabaseContext
  {
    std::string definitions_directory = "definitions";
    std::string client_build;
    bool prefer_db2 = false;
  };

  //! One "LAYOUT/BUILD + field list" block of a .dbd definition file.
  struct DefinitionBlock
  {
    std::vector<std::uint32_t> layout_hashes;
    std::vector<std::string> builds;
    int field_count = 0;
  };

  //! Header of a client database file (DBC/WDC1..WDC5), read without any
  //! definition so it can be used to pick a definition.
  struct ClientDatabaseFileHeader
  {
    bool valid = false;
    std::string magic;
    std::uint32_t layout_hash = 0;
    int fields_count = 0;
    //! WDC1-3 only: field_count plus the columns carried solely in the common
    //! data block. This is the count a .dbd definition lists, while
    //! fields_count only covers the columns packed into the record.
    int total_fields_count = 0;

    int effective_fields_count() const
    {
      return total_fields_count > 0 ? total_fields_count : fields_count;
    }
    int records_count = 0;
    int record_size = 0;
  };

  using ClientDatabaseFileCallback =
    std::function<std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream>(std::string const&)>;

  //! Parses every version block of a .dbd file. Returns an empty vector when the
  //! file cannot be read.
  std::vector<DefinitionBlock> parseDefinitionBlocks(std::filesystem::path const& dbd_file);

  //! Extracts the header of a DBC/DB2 image. `data` must point at the file magic.
  ClientDatabaseFileHeader readClientDatabaseHeader(char const* data, std::size_t size);

  //! Orders the builds declared by `blocks` by how well they describe `header`:
  //! exact layout hash matches first, then blocks with a matching field count,
  //! then blocks from the same expansion. Ties are broken by proximity to
  //! `client_build`.
  std::vector<std::string> orderCandidateBuilds(std::vector<DefinitionBlock> const& blocks,
                                                ClientDatabaseFileHeader const& header,
                                                std::string const& client_build,
                                                std::size_t max_candidates);

  //! Loads `table_name` through `primary`, substituting a definition when the
  //! detected client build is not described by the shipped DBD files.
  //!
  //! Modern clients ship DB2 files whose layout is not always described by the
  //! definitions bundled for the detected build: retail keeps changing layouts
  //! while Noggit's .dbd files only list the builds they were verified against.
  //! Instead of giving up - and instead of logging the same failure hundreds of
  //! times per session - this retries the load with the definition block that best
  //! matches the file on disk and caches the result for the session.
  //!
  //! Throws std::runtime_error when no definition can describe the file.
  BlizzardDatabaseLib::BlizzardDatabaseTable& loadTableWithDefinitionFallback(
    BlizzardDatabaseLib::BlizzardDatabase& primary,
    std::string const& table_name,
    ClientDatabaseContext const& context,
    ClientDatabaseFileCallback const& file_callback,
    char const* log_context);

  //! Definition that was substituted for `table_name`, or nullptr when the table
  //! was loaded through the project's own build.
  BlizzardDatabaseLib::Structures::VersionDefinition* substitutedDefinition(
    std::string const& table_name);

  //! Number of tables that had to be loaded through a substituted definition.
  std::size_t substitutedDefinitionCount();
}

#endif //NOGGIT_DB2_DEFINITION_FALLBACK_HPP
