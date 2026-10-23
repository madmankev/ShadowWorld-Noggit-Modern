// Export pipeline core. See the header for scope and policy. Qt-free,
// dependency-free; unit tested by tools/adt_format_check.cpp.

#include <noggit/formats/export/MapExport.hpp>

#include <noggit/formats/adt/ModernAdtWriter.hpp>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>

namespace Noggit::Formats::Export
{
  namespace
  {
    constexpr std::uint32_t mexp_fc(char a, char b, char c, char d) noexcept
    {
      return (std::uint32_t(static_cast<unsigned char>(a)) << 24)
           | (std::uint32_t(static_cast<unsigned char>(b)) << 16)
           | (std::uint32_t(static_cast<unsigned char>(c)) << 8)
           | std::uint32_t(static_cast<unsigned char>(d));
    }

    constexpr std::uint32_t MEXP_MVER = mexp_fc('M', 'V', 'E', 'R');
    constexpr std::uint32_t MEXP_MPHD = mexp_fc('M', 'P', 'H', 'D');
    constexpr std::uint32_t MEXP_MAIN = mexp_fc('M', 'A', 'I', 'N');
    constexpr std::uint32_t MEXP_MAID = mexp_fc('M', 'A', 'I', 'D');
    constexpr std::uint32_t MEXP_MWMO = mexp_fc('M', 'W', 'M', 'O');
    constexpr std::uint32_t MEXP_MWID = mexp_fc('M', 'W', 'I', 'D');
    constexpr std::uint32_t MEXP_MODF = mexp_fc('M', 'O', 'D', 'F');

    //! MPHD flag: the client loads ADTs through the MAID FileDataID table.
    constexpr std::uint32_t MEXP_MPHD_HAS_MAID = 0x200;
    //! MPHD flag: the client loads _lod.adt parts.
    constexpr std::uint32_t MEXP_MPHD_HAS_LOD = 0x100;

    struct mexp_chunk
    {
      std::uint32_t magic;
      std::size_t offset;
      std::size_t size;
    };

    bool mexp_walk(std::vector<std::uint8_t> const& bytes,
                   std::vector<mexp_chunk>& out, std::string* error)
    {
      std::size_t pos = 0;
      while (pos + 8 <= bytes.size())
      {
        mexp_chunk chunk{};
        std::memcpy(&chunk.magic, bytes.data() + pos, 4);
        std::memcpy(&chunk.size, bytes.data() + pos + 4, 4);
        chunk.offset = pos;
        if (chunk.size > bytes.size() - pos - 8)
        {
          if (error)
            *error = "truncated chunk at offset " + std::to_string(pos);
          return false;
        }
        out.push_back(chunk);
        pos += 8 + chunk.size;
      }
      if (pos != bytes.size())
      {
        if (error)
          *error = "the walk ends " + std::to_string(bytes.size() - pos)
                 + " byte(s) inside a chunk tag";
        return false;
      }
      return true;
    }

    std::uint32_t mexp_read_u32(std::vector<std::uint8_t> const& bytes, std::size_t at)
    {
      std::uint32_t v = 0;
      std::memcpy(&v, bytes.data() + at, 4);
      return v;
    }

    void mexp_write_u32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t v)
    {
      std::memcpy(bytes.data() + at, &v, 4);
    }

    //! Internal path key: lowercase with backslashes, the convention the
    //! client file APIs normalize to.
    std::string mexp_normalize(std::string path)
    {
      for (char& c : path)
      {
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c - 'A' + 'a');
        if (c == '/')
          c = '\\';
      }
      return path;
    }

    //! Artifact path form: lowercase with forward slashes, the listfile
    //! convention.
    std::string mexp_listfile_path(std::string const& normalized)
    {
      std::string path = normalized;
      for (char& c : path)
        if (c == '\\')
          c = '/';
      return path;
    }

    std::string mexp_json_escape(std::string const& text)
    {
      std::string out;
      out.reserve(text.size() + 8);
      for (char const c : text)
      {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20)
          {
            std::ostringstream hex;
            hex << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                << static_cast<int>(static_cast<unsigned char>(c));
            out += hex.str();
          }
          else
            out += c;
        }
      }
      return out;
    }

    //! Content hash for the manifest: FNV-1a 64. Not cryptographic - it is
    //! a change detector for re-exports, like the manifest's sizes.
    std::uint64_t mexp_fnv1a64(std::vector<std::uint8_t> const& bytes)
    {
      std::uint64_t hash = 1469598103934665603ull;
      for (std::uint8_t const b : bytes)
      {
        hash ^= b;
        hash *= 1099511628211ull;
      }
      return hash;
    }

    //! The MPHD FileDataID columns: word index -> aux WDT suffix. Word 6 is
    //! the WDL and is handled with the WDL itself.
    struct mexp_aux_slot { char const* suffix; int word; };
    constexpr mexp_aux_slot MEXP_AUX_SLOTS[] = {
      {"_lgt", 1}, {"_occ", 2}, {"_fogs", 3}, {"_mpv", 4}, {"_tex", 5}, {"_pd4", 7},
    };
  }

  ExportResult exportModernMap(ExportOptions const& options, ExportInput const& input)
  {
    ExportResult result;
    result.options = options;
    result.export_generation = options.export_generation;

    if (options.export_generation == Conversion::MapGeneration::wotlk_3_3_5)
    {
      result.errors.push_back("export packages the modern families; re-authoring"
                              " the ADT/WDT/WDL layouts for WotLK is the Convert"
                              " map tool's job, not the exporter's");
      return result;
    }

    std::string const dir = mexp_normalize(options.map_directory);
    std::string const base = "world\\maps\\" + dir + "\\" + dir;
    std::string const wdt_path = base + ".wdt";

    // ---- index the export set ----
    std::map<std::string, std::size_t> file_index;
    for (std::size_t i = 0; i < input.files.size(); ++i)
    {
      std::string const path = mexp_normalize(input.files[i].relative_path);
      auto const inserted = file_index.emplace(path, i);
      if (!inserted.second)
        result.warnings.push_back("duplicate path in the export set: " + path
                                  + " (keeping the first)");
    }

    auto find_file = [&](std::string const& path) -> ExportFile const*
    {
      auto const it = file_index.find(path);
      return it == file_index.end() ? nullptr : &input.files[it->second];
    };

    auto known_fdid = [&](std::string const& path) -> std::uint32_t
    {
      return input.path_to_fdid ? input.path_to_fdid(path) : 0u;
    };

    ExportFile const* wdt_file = find_file(wdt_path);
    if (!wdt_file)
    {
      result.errors.push_back("the export set has no WDT (" + wdt_path
                              + "); a modern client loads every tile through it");
      return result;
    }

    // ---- parse the WDT ----
    std::vector<std::uint8_t> wdt = wdt_file->bytes; // patched in place below
    std::vector<mexp_chunk> chunks;
    std::string walk_error;
    if (!mexp_walk(wdt, chunks, &walk_error))
    {
      result.errors.push_back("the WDT does not walk: " + walk_error);
      return result;
    }

    std::size_t mphd_payload = 0;
    std::size_t main_payload = 0;
    std::size_t maid_payload = 0;
    std::uint32_t version = 0;
    bool have_mphd = false, have_main = false, have_maid = false;
    for (auto const& chunk : chunks)
    {
      if (chunk.magic == MEXP_MVER && chunk.size >= 4 && !version)
        version = mexp_read_u32(wdt, chunk.offset + 8);
      else if (chunk.magic == MEXP_MPHD && !have_mphd)
      {
        mphd_payload = chunk.offset + 8;
        have_mphd = chunk.size >= 32;
        if (!have_mphd)
          result.errors.push_back("the WDT's MPHD chunk is smaller than 32 bytes;"
                                  " a modern WDT carries the flags word plus the"
                                  " FileDataID columns");
      }
      else if (chunk.magic == MEXP_MAIN && !have_main)
      {
        main_payload = chunk.offset + 8;
        have_main = chunk.size >= 4096 * 8;
        if (!have_main)
          result.errors.push_back("the WDT's MAIN chunk is smaller than 4096 entries");
      }
      else if (chunk.magic == MEXP_MAID && !have_maid)
      {
        maid_payload = chunk.offset + 8;
        have_maid = chunk.size >= 4096 * 32;
        if (!have_maid)
          result.errors.push_back("the WDT's MAID chunk is smaller than 4096 entries");
      }
    }

    if (version != 18)
      result.warnings.push_back("the WDT reports MVER " + std::to_string(version)
                                + "; modern WDTs are version 18");
    if (!have_mphd || !have_main)
      return result; // errors recorded above

    std::uint32_t const mphd_flags = mexp_read_u32(wdt, mphd_payload);
    bool const client_loads_by_fdid = (mphd_flags & MEXP_MPHD_HAS_MAID) != 0;
    if (client_loads_by_fdid && !have_maid)
    {
      result.errors.push_back("the WDT's MPHD flags announce MAID (0x200) but the"
                              " file has no usable MAID chunk; the client would"
                              " load tiles by FileDataIDs that do not exist");
      return result;
    }
    if (!client_loads_by_fdid)
      result.warnings.push_back("the WDT's MPHD flags lack 0x200 (wdt_has_maid);"
                                " pre-8.1 clients load ADTs by path - MAID"
                                " assignments are skipped");

    // ---- FileDataID bookkeeping ----
    // Every id already spoken for in this WDT is reserved: allocations must
    // not collide with the client's own table entries.
    std::set<std::uint32_t> used_ids;
    for (int word = 1; word < 8; ++word)
      if (std::uint32_t const id = mexp_read_u32(wdt, mphd_payload + word * 4))
        used_ids.insert(id);
    if (have_maid)
      for (std::size_t i = 0; i < 4096 * 8; ++i)
        if (std::uint32_t const id = mexp_read_u32(wdt, maid_payload + i * 4))
          used_ids.insert(id);
    for (auto const& file : input.files)
      if (std::uint32_t const id = known_fdid(mexp_normalize(file.relative_path)))
        used_ids.insert(id);

    std::uint32_t next_fdid = options.fdid_base ? options.fdid_base : 1u;
    auto allocate_fdid = [&]() -> std::uint32_t
    {
      while (used_ids.count(next_fdid))
        ++next_fdid;
      used_ids.insert(next_fdid);
      return next_fdid;
    };

    auto record = [&](std::uint32_t fdid, std::string const& path,
                      char const* role, bool existed)
    {
      Conversion::FdidAssignment assignment;
      assignment.fdid = fdid;
      assignment.path = mexp_listfile_path(path);
      assignment.role = role;
      assignment.existed_in_client = existed;
      result.fdid_assignments.push_back(std::move(assignment));
    };

    // ---- tiles: inventory, validation, MAID assignments ----
    struct part_shape
    {
      char const* suffix;
      int maid_slot;               // -1: not in MAID
      ADT::AdtPartExpectation expectation;
      char const* role;
      bool required;
    };
    constexpr part_shape part_shapes[] = {
      {"", 0, ADT::AdtPartExpectation::Root, "adt_root", true},
      {"_tex0", 3, ADT::AdtPartExpectation::Texture, "adt_tex0", true},
      {"_obj0", 1, ADT::AdtPartExpectation::Object, "adt_obj0", true},
      {"_obj1", 2, ADT::AdtPartExpectation::Generic, "adt_obj1", false},
      {"_lod", 4, ADT::AdtPartExpectation::Generic, "adt_lod", false},
    };

    auto const& profile = ADT::adt_profile_v18();
    unsigned int tiles_without_baked_textures = 0;

    for (std::size_t i = 0; i < 4096; ++i)
    {
      std::uint32_t const tile_flags = mexp_read_u32(wdt, main_payload + i * 8);
      if (!(tile_flags & 0x1))
        continue;
      ++result.tiles_total;

      std::size_t const errors_before = result.errors.size();
      unsigned int x = static_cast<unsigned int>(i % 64);
      unsigned int y = static_cast<unsigned int>(i / 64);
      std::string const tile_base =
        base + "_" + std::to_string(x) + "_" + std::to_string(y);

      bool tile_is_new = false;
      for (auto const& shape : part_shapes)
      {
        std::string const path = tile_base + shape.suffix + ".adt";
        ExportFile const* file = find_file(path);

        if (!file)
        {
          if (shape.required)
            result.errors.push_back("tile (" + std::to_string(x) + ","
                                    + std::to_string(y) + ") is missing " + path);
          else if (shape.maid_slot == 4 && !(mphd_flags & MEXP_MPHD_HAS_LOD))
            ; // _lod parts are only loaded when MPHD says so
          else
            result.warnings.push_back("tile (" + std::to_string(x) + ","
                                      + std::to_string(y) + ") has no " + path);
          continue;
        }

        for (auto const& problem : ADT::ModernAdtWriter::validatePart(profile, file->bytes,
                                                                      shape.expectation))
          result.errors.push_back(path + " failed validation: " + problem);

        if (!client_loads_by_fdid || !have_maid || shape.maid_slot < 0)
          continue;

        std::size_t const slot_offset = maid_payload + i * 32 + shape.maid_slot * 4;
        std::uint32_t const current = mexp_read_u32(wdt, slot_offset);
        if (current)
        {
          record(current, path, shape.role, true);
          continue;
        }

        std::uint32_t const id = known_fdid(path);
        if (id)
        {
          mexp_write_u32(wdt, slot_offset, id);
          record(id, path, shape.role, true);
        }
        else
        {
          std::uint32_t const allocated = allocate_fdid();
          mexp_write_u32(wdt, slot_offset, allocated);
          record(allocated, path, shape.role, false);
          tile_is_new = true;
        }
      }

      if (tile_is_new)
      {
        // MAID slots 5-7 are the client-baked map texture, its normals and
        // the minimap image. Noggit cannot author those offline; they stay
        // zero and are reported once for the map instead of per tile.
        bool baked_zero = true;
        for (int slot = 5; slot < 8; ++slot)
          baked_zero = baked_zero && !mexp_read_u32(wdt, maid_payload + i * 32 + slot * 4);
        if (baked_zero)
          ++tiles_without_baked_textures;
      }

      if (result.errors.size() == errors_before)
        ++result.tiles_complete;
    }

    if (tiles_without_baked_textures)
      result.warnings.push_back(std::to_string(tiles_without_baked_textures)
                                + " newly assigned tile(s) keep zero baked map"
                                  " texture / normals / minimap FileDataIDs"
                                  " (MAID slots 5-7); those images are"
                                  " client-baked and cannot be authored offline");

    // ---- WDL ----
    std::string const wdl_path = base + ".wdl";
    if (ExportFile const* wdl_file = find_file(wdl_path))
    {
      std::vector<mexp_chunk> wdl_chunks;
      std::string wdl_error;
      if (!mexp_walk(wdl_file->bytes, wdl_chunks, &wdl_error))
        result.warnings.push_back("the WDL does not walk (" + wdl_error + ")");
      else
        for (auto const& chunk : wdl_chunks)
          if (chunk.magic == MEXP_MWMO || chunk.magic == MEXP_MWID
              || chunk.magic == MEXP_MODF)
          {
            result.warnings.push_back("the WDL carries legacy object chunks"
                                      " (MWMO/MWID/MODF); modern clients read"
                                      " MLMD/MLMX - regenerate the WDL so it is"
                                      " written in the modern format");
            break;
          }

      // MPHD word 6 is the WDL's own FileDataID.
      std::uint32_t const current = mexp_read_u32(wdt, mphd_payload + 6 * 4);
      if (current)
        record(current, wdl_path, "wdl", true);
      else if (std::uint32_t const id = known_fdid(wdl_path))
      {
        mexp_write_u32(wdt, mphd_payload + 6 * 4, id);
        record(id, wdl_path, "wdl", true);
      }
      else
      {
        std::uint32_t const allocated = allocate_fdid();
        mexp_write_u32(wdt, mphd_payload + 6 * 4, allocated);
        record(allocated, wdl_path, "wdl", false);
      }
    }
    else
    {
      result.warnings.push_back("the export set has no WDL (" + wdl_path + ");"
                                " the client's low-detail horizon comes from it"
                                " (Editor menu: Generate new WDL)");
    }

    // ---- aux WDTs referenced through the MPHD columns ----
    for (auto const& slot : MEXP_AUX_SLOTS)
    {
      std::string const path = base + slot.suffix + ".wdt";
      std::uint32_t const current = mexp_read_u32(wdt, mphd_payload + slot.word * 4);
      std::string const role = std::string("aux_wdt") + slot.suffix;

      if (ExportFile const* file = find_file(path))
      {
        (void)file;
        if (current)
          record(current, path, role.c_str(), true);
        else if (std::uint32_t const id = known_fdid(path))
        {
          mexp_write_u32(wdt, mphd_payload + slot.word * 4, id);
          record(id, path, role.c_str(), true);
        }
        else
        {
          std::uint32_t const allocated = allocate_fdid();
          mexp_write_u32(wdt, mphd_payload + slot.word * 4, allocated);
          record(allocated, path, role.c_str(), false);
        }
      }
      else if (current)
      {
        result.warnings.push_back("MPHD references " + path + " (FileDataID "
                                  + std::to_string(current) + ") but the export"
                                  " set does not contain it");
      }
    }

    // ---- the WDT's own FileDataID (Map.db2 WdtFileDataID) ----
    if (std::uint32_t const id = known_fdid(wdt_path))
    {
      result.wdt_fdid = id;
      record(id, wdt_path, "wdt_file", true);
    }
    else
    {
      result.wdt_fdid = allocate_fdid();
      record(result.wdt_fdid, wdt_path, "wdt_file", false);
    }

    // ---- generation step: detect the family the source bytes belong to and
    // step the map-level files to the selected export family ----
    std::vector<std::uint8_t> wdl_original;
    if (ExportFile const* wdl_file = find_file(wdl_path))
      wdl_original = wdl_file->bytes;
    result.source_generation = Conversion::detectModernGeneration(wdt, wdl_original);

    std::vector<std::uint8_t> wdl_final;
    bool have_wdl_final = false;
    if (result.source_generation != options.export_generation)
    {
      auto const& from_label = Conversion::generationFeatures(result.source_generation).label;
      auto const& to_label = Conversion::generationFeatures(options.export_generation).label;

      auto wdt_step = Conversion::convertWdtBetweenGenerations(
        wdt, result.source_generation, options.export_generation);
      for (auto const& note : wdt_step.notes)
        result.warnings.push_back("WDT generation step (" + std::string(from_label)
                                  + " -> " + to_label + "): " + note);
      if (wdt_step.ok)
        wdt = std::move(wdt_step.bytes);
      else
        result.errors.push_back("the WDT could not be stepped from "
                                + std::string(from_label) + " to " + to_label);

      if (!wdl_original.empty())
      {
        auto wdl_step = Conversion::convertWdlBetweenGenerations(
          wdl_original, result.source_generation, options.export_generation);
        for (auto const& note : wdl_step.notes)
          result.warnings.push_back("WDL generation step (" + std::string(from_label)
                                    + " -> " + to_label + "): " + note);
        if (wdl_step.ok)
        {
          wdl_final = std::move(wdl_step.bytes);
          have_wdl_final = true;
        }
        else
        {
          result.errors.push_back("the WDL could not be stepped from "
                                  + std::string(from_label) + " to " + to_label);
        }
      }
    }

    // ---- assemble the deployable set: input files, WDT (and a stepped WDL)
    // replaced ----
    result.files = input.files;
    for (auto& file : result.files)
    {
      std::string const path = mexp_normalize(file.relative_path);
      if (path == wdt_path)
        file.bytes = wdt;
      else if (have_wdl_final && path == wdl_path)
        file.bytes = wdl_final;
    }

    result.ok = result.errors.empty();
    return result;
  }

  std::string ExportResult::manifest_json() const
  {
    std::size_t new_ids = 0;
    for (auto const& assignment : fdid_assignments)
      if (!assignment.existed_in_client)
        ++new_ids;

    std::ostringstream out;
    out << "{\n";
    out << "  \"format_version\": 1,\n";
    out << "  \"kind\": \"map_export\",\n";
    out << "  \"map_directory\": \"" << mexp_json_escape(options.map_directory) << "\",\n";
    out << "  \"map_name\": \""
        << mexp_json_escape(options.map_display_name.empty()
                              ? options.map_directory : options.map_display_name)
        << "\",\n";
    out << "  \"map_id\": " << options.map_id << ",\n";
    out << "  \"target_build\": \"" << mexp_json_escape(options.target_build) << "\",\n";
    out << "  \"source_generation\": \""
        << Conversion::generationFeatures(source_generation).label << "\",\n";
    out << "  \"export_generation\": \""
        << Conversion::generationFeatures(export_generation).label << "\",\n";
    out << "  \"stats\": {\n";
    out << "    \"tiles_total\": " << tiles_total << ",\n";
    out << "    \"tiles_complete\": " << tiles_complete << ",\n";
    out << "    \"files\": " << files.size() << ",\n";
    out << "    \"new_fdids\": " << new_ids << "\n";
    out << "  },\n";

    out << "  \"files\": [\n";
    for (std::size_t i = 0; i < files.size(); ++i)
    {
      std::ostringstream hash;
      hash << std::hex << std::setw(16) << std::setfill('0') << mexp_fnv1a64(files[i].bytes);
      out << "    {\"path\": \"" << mexp_json_escape(mexp_listfile_path(mexp_normalize(files[i].relative_path)))
          << "\", \"size\": " << files[i].bytes.size()
          << ", \"fnv1a64\": \"" << hash.str() << "\"}"
          << (i + 1 < files.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"fdid_assignments\": [\n";
    for (std::size_t i = 0; i < fdid_assignments.size(); ++i)
    {
      auto const& a = fdid_assignments[i];
      out << "    {\"fdid\": " << a.fdid
          << ", \"path\": \"" << mexp_json_escape(a.path)
          << "\", \"role\": \"" << mexp_json_escape(a.role)
          << "\", \"existed_in_client\": " << (a.existed_in_client ? "true" : "false") << "}"
          << (i + 1 < fdid_assignments.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    auto const fields = registration_fields();
    out << "  \"registration\": {\n";
    out << "    \"id\": " << options.map_id << ",\n";
    out << "    \"target_build\": \"" << mexp_json_escape(options.target_build) << "\",\n";
    out << "    \"fields\": {\n";
    std::size_t n = 0;
    for (auto const& field : fields)
    {
      out << "      \"" << mexp_json_escape(field.first) << "\": \""
          << mexp_json_escape(field.second) << "\""
          << (++n < fields.size() ? "," : "") << "\n";
    }
    out << "    }\n";
    out << "  },\n";

    out << "  \"warnings\": [\n";
    for (std::size_t i = 0; i < warnings.size(); ++i)
      out << "    \"" << mexp_json_escape(warnings[i]) << "\""
          << (i + 1 < warnings.size() ? "," : "") << "\n";
    out << "  ],\n";
    out << "  \"errors\": [\n";
    for (std::size_t i = 0; i < errors.size(); ++i)
      out << "    \"" << mexp_json_escape(errors[i]) << "\""
          << (i + 1 < errors.size() ? "," : "") << "\n";
    out << "  ]\n";
    out << "}\n";
    return out.str();
  }

  std::string ExportResult::listfile_csv() const
  {
    std::vector<Conversion::FdidAssignment const*> fresh;
    for (auto const& assignment : fdid_assignments)
      if (!assignment.existed_in_client)
        fresh.push_back(&assignment);
    std::stable_sort(fresh.begin(), fresh.end(),
                     [](Conversion::FdidAssignment const* a, Conversion::FdidAssignment const* b)
                     { return a->fdid < b->fdid; });

    std::ostringstream out;
    for (auto const* assignment : fresh)
      out << assignment->fdid << ";" << assignment->path << "\n";
    return out.str();
  }

  std::map<std::string, std::string> ExportResult::registration_fields() const
  {
    std::map<std::string, std::string> fields;
    fields["Directory"] = options.map_directory;
    fields["MapName_lang"] = options.map_display_name.empty()
                               ? options.map_directory : options.map_display_name;
    if (wdt_fdid)
      fields["WdtFileDataID"] = std::to_string(wdt_fdid);
    return fields;
  }
}
