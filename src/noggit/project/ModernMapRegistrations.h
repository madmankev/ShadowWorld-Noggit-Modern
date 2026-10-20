#pragma once
// Project-local registration of newly created maps for modern (DB2-based)
// clients (issue: "Add Map.db2 registration for new maps").
//
// Legacy projects persist wizard-created maps by rewriting
// DBFilesClient/Map.dbc inside the project folder. On modern projects that
// file is dead data: the DB2 bridge rebuilds the editor's map table from the
// client's Map.db2 on every load, so newly created maps vanished after a
// restart. These registrations are the modern equivalent:
//
//   * the map wizard stores the new map's Map.db2 column values - plus the
//     client build they were authored against - in
//     <project>/extraData/modernMapRegistrations.json
//   * the DB2 bridge merges them into the bridged table after the CASC rows,
//     so registered maps survive reloads
//   * merges are version-aware: values live under modern column names, and
//     columns the loaded build does not know (or the registration does not
//     carry) fall back to the bridge converter's defaults
//
// The reader/writer is deliberately dependency-free (a small hand-rolled JSON
// codec for this one schema) so the storage and validation paths can be
// unit-tested outside the application.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

class DBCFile;

namespace Noggit::Project
{
  struct ModernMapRegistration
  {
    int id = -1;
    // Client build the fields were authored for, e.g. "9.2.7.45745". Empty on
    // hand-authored files; merges still work via converter defaults.
    std::string target_build;
    // Map.db2 column name -> raw value (numbers are stored in string form).
    std::map<std::string, std::string> fields;
    // Array-typed columns, e.g. "Corpse" -> {x, y}.
    std::map<std::string, std::vector<std::string>> array_fields;
  };

  namespace ModernMapRegistrations
  {
    inline constexpr int storage_format_version = 1;

    std::filesystem::path storage_path(std::filesystem::path const& project_path);

    // Never throws: returns {} and logs on malformed or unreadable files.
    std::vector<ModernMapRegistration> load(std::filesystem::path const& project_path);

    // Creates <project>/extraData when needed. Returns false and logs on
    // failure. Writes atomically (temporary file + rename).
    bool save(std::filesystem::path const& project_path,
              std::vector<ModernMapRegistration> const& registrations);

    // Replaces the entry with the same id, or appends it.
    void upsert(std::vector<ModernMapRegistration>& registrations,
                ModernMapRegistration registration);

    // Removes the entry with the given id. Returns true when something was
    // removed.
    bool remove(std::vector<ModernMapRegistration>& registrations, int id);

    // Returns an empty string when the registration is acceptable, otherwise a
    // human-readable reason. Enforces the fields a modern map cannot work
    // without; the set mirrors the Map.dbd columns of the first targeted
    // modern build (Shadowlands 9.2.7) that the bridge converter reads.
    std::string validate(ModernMapRegistration const& registration);

    // Writes the registration into a legacy-format Map DBC record - the shape
    // the DB2 bridge produces - creating the record when missing. Returns
    // false when the id already exists in the table (client data wins).
    bool apply_to_map_dbc(DBCFile& map_dbc, ModernMapRegistration const& registration);
  }
}
